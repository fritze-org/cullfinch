// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/AssociationPolicy.h>

#include <cullfinch/domain/NaturalOrder.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHash>
#include <QMap>

#include <algorithm>

namespace cullfinch::domain {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

/// Key used to detect collisions a case-insensitive or normalising filesystem
/// would conflate. Never used to merge assets: only to flag them.
QString foldedKey(const QString& relativeDirectory, const QString& stem) {
    return relativeDirectory.normalized(QString::NormalizationForm_C).toCaseFolded() +
           QLatin1Char('\x1f') + stem.normalized(QString::NormalizationForm_C).toCaseFolded();
}

QString exactKey(const QString& relativeDirectory, const QString& stem) {
    return relativeDirectory + QLatin1Char('\x1f') + stem;
}

int roleRank(MemberRole role) {
    switch (role) {
    case MemberRole::Jpeg:
        return 0;
    case MemberRole::Raw:
        return 1;
    case MemberRole::Sidecar:
        return 2;
    case MemberRole::Unknown:
        break;
    }
    return 3;
}

} // namespace

AssociationConfig AssociationConfig::defaults() {
    AssociationConfig config;
    config.jpegExtensions = QStringList{QStringLiteral("jpg"), QStringLiteral("jpeg")};
    // Common camera RAW filenames. Recognising a name is not a decoding claim.
    config.rawExtensions = QStringList{
        QStringLiteral("cr2"), QStringLiteral("cr3"), QStringLiteral("nef"), QStringLiteral("nrw"),
        QStringLiteral("arw"), QStringLiteral("raf"), QStringLiteral("orf"), QStringLiteral("rw2"),
        QStringLiteral("pef"), QStringLiteral("srw"), QStringLiteral("dng"), QStringLiteral("raw")};
    config.sidecarExtensions = QStringList{QStringLiteral("xmp")};
    config.sidecarsEnabled = false;
    return config;
}

MemberRole AssociationConfig::roleFor(const QString& extension) const {
    const QString folded = extension.toCaseFolded();
    if (jpegExtensions.contains(folded)) {
        return MemberRole::Jpeg;
    }
    if (rawExtensions.contains(folded)) {
        return MemberRole::Raw;
    }
    if (sidecarsEnabled && sidecarExtensions.contains(folded)) {
        return MemberRole::Sidecar;
    }
    return MemberRole::Unknown;
}

bool AssociationConfig::isDisabledSidecar(const QString& extension) const {
    return !sidecarsEnabled && sidecarExtensions.contains(extension.toCaseFolded());
}

QString StemAssociationResolver::id() const {
    return QStringLiteral("same-directory-exact-stem");
}

QString StemAssociationResolver::displayName() const {
    return tr("Same directory, identical filename stem");
}

AssetId StemAssociationResolver::deriveAssetId(const CollectionId& collectionId,
                                               const QString& relativeDirectory,
                                               const QString& stem) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(collectionId.toString().toUtf8());
    hash.addData(QByteArrayLiteral("\x1f"));
    hash.addData(relativeDirectory.toUtf8());
    hash.addData(QByteArrayLiteral("\x1f"));
    hash.addData(stem.toUtf8());
    return AssetId(QString::fromLatin1(hash.result().toHex()));
}

MemberId StemAssociationResolver::deriveMemberId(const QString& absolutePath) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(absolutePath.toUtf8());
    return MemberId(QString::fromLatin1(hash.result().toHex()));
}

AssociationResult StemAssociationResolver::resolve(const CollectionId& collectionId,
                                                   const QList<DiscoveredFile>& files,
                                                   const AssociationConfig& config,
                                                   bool scopeComplete) const {
    AssociationResult result;

    // ---- 1. Bucket by the exact (directory, stem) key -----------------------
    // QMap keeps a deterministic key order; the final asset order is decided by
    // natural filename comparison below.
    QMap<QString, QList<DiscoveredFile>> groups;
    QHash<QString, QStringList> foldedToExact;

    for (const DiscoveredFile& file : files) {
        if (config.isDisabledSidecar(file.extension)) {
            // A recognised sidecar candidate with sidecar handling switched
            // off: ignored, not unclassified.
            continue;
        }
        const QString key = exactKey(file.relativeDirectory, file.stem);
        groups[key].append(file);

        QStringList& exactKeys = foldedToExact[foldedKey(file.relativeDirectory, file.stem)];
        if (!exactKeys.contains(key)) {
            exactKeys.append(key);
        }
    }

    // ---- 2. Detect hardlink aliases across the whole scan -------------------
    QHash<QString, QString> nativeIdentityOwner; // identity -> absolute path
    QHash<QString, bool> aliasedPath;
    for (const DiscoveredFile& file : files) {
        const NativeIdentity& native = file.fingerprint.native;
        if (!native.known) {
            continue;
        }
        const QString identity = QStringLiteral("%1:%2").arg(native.device).arg(native.fileId);
        const auto existing = nativeIdentityOwner.constFind(identity);
        if (existing != nativeIdentityOwner.constEnd() && *existing != file.absolutePath) {
            aliasedPath[file.absolutePath] = true;
            aliasedPath[*existing] = true;
            result.diagnostics.append(tr("Hardlink alias: '%1' and '%2' are the same file.")
                                          .arg(*existing, file.absolutePath));
        } else {
            nativeIdentityOwner.insert(identity, file.absolutePath);
        }
    }

    // ---- 3. Build one asset per exact key -----------------------------------
    for (auto group = groups.cbegin(); group != groups.cend(); ++group) {
        const QList<DiscoveredFile>& entries = group.value();
        const DiscoveredFile& first = entries.first();

        PhotoAsset asset;
        asset.collectionId = collectionId;
        asset.id = deriveAssetId(collectionId, first.relativeDirectory, first.stem);
        asset.stem = first.stem;
        asset.relativeDirectory = first.relativeDirectory;
        asset.displayName = first.relativeDirectory.isEmpty()
                                ? first.stem
                                : first.relativeDirectory + QLatin1Char('/') + first.stem;

        int jpegCount = 0;
        int rawCount = 0;
        int unknownCount = 0;
        MemberId firstJpegId;

        for (const DiscoveredFile& entry : entries) {
            FileMember member;
            member.id = deriveMemberId(entry.absolutePath);
            member.role = config.roleFor(entry.extension);
            member.absolutePath = entry.absolutePath;
            member.fileName = entry.fileName;
            member.extensionLower = entry.extension.toCaseFolded();
            member.fingerprint = entry.fingerprint;
            member.isSymlink = entry.isSymlink;

            switch (member.role) {
            case MemberRole::Jpeg:
                ++jpegCount;
                if (!firstJpegId.isValid()) {
                    firstJpegId = member.id;
                }
                break;
            case MemberRole::Raw:
                ++rawCount;
                break;
            case MemberRole::Unknown:
                ++unknownCount;
                asset.diagnostics.append(
                    tr("Unclassified file with the same stem: '%1'. Classify it before file "
                       "operations on this photo.")
                        .arg(entry.fileName));
                // Blocks operations, not browsing.
                asset.operationsBlocked = true;
                break;
            case MemberRole::Sidecar:
                break;
            }

            if (entry.isSymlink) {
                asset.diagnostics.append(
                    tr("'%1' is a symbolic link. Symlinks are not followed for file operations.")
                        .arg(entry.fileName));
                asset.operationsBlocked = true;
            }
            if (aliasedPath.value(entry.absolutePath, false)) {
                asset.diagnostics.append(
                    tr("'%1' is a hardlink alias of another discovered file.").arg(entry.fileName));
                asset.operationsBlocked = true;
            }

            asset.members.append(member);
        }

        // Preview first, then RAW companions, then sidecars, then unclassified.
        std::stable_sort(asset.members.begin(), asset.members.end(),
                         [](const FileMember& lhs, const FileMember& rhs) {
                             if (roleRank(lhs.role) != roleRank(rhs.role)) {
                                 return roleRank(lhs.role) < roleRank(rhs.role);
                             }
                             return naturalLess(lhs.fileName, rhs.fileName);
                         });

        // ---- 4. Pairing state ----------------------------------------------
        if (jpegCount > 1) {
            asset.pairingState = PairingState::Ambiguous;
            asset.diagnostics.append(
                tr("%1 candidate previews share this stem. Resolve which one represents the "
                   "photo before comparing or operating on it.")
                    .arg(jpegCount));
        } else if (jpegCount == 1) {
            asset.previewMemberId = firstJpegId;
            asset.pairingState = rawCount > 0 ? PairingState::Resolved : PairingState::JpegOnly;
        } else if (rawCount > 0) {
            asset.pairingState = PairingState::RawOnly;
            asset.diagnostics.append(
                tr("No JPG preview accompanies this RAW file. cullfinch does not render RAW "
                   "embedded previews."));
        } else {
            asset.pairingState = PairingState::Ambiguous;
            asset.diagnostics.append(tr("No recognised image file with this stem."));
        }

        if (unknownCount > 0 && asset.pairingState == PairingState::Resolved) {
            // Membership is still resolved; only operations are held back.
            asset.diagnostics.append(tr("Comparison is unaffected; file operations are blocked."));
        }

        // A group whose association scope is still being enumerated must not be
        // classified yet, nor take part in comparisons or operations.
        if (!scopeComplete) {
            asset.pairingState = PairingState::Provisional;
        }

        result.assets.append(asset);
    }

    // ---- 5. Flag case-folding and normalisation collisions ------------------
    QHash<QString, PhotoAsset*> assetByExactKey;
    for (PhotoAsset& asset : result.assets) {
        assetByExactKey.insert(exactKey(asset.relativeDirectory, asset.stem), &asset);
    }

    for (auto folded = foldedToExact.cbegin(); folded != foldedToExact.cend(); ++folded) {
        const QStringList& exactKeys = folded.value();
        if (exactKeys.size() < 2) {
            continue;
        }
        QStringList stems;
        for (const QString& key : exactKeys) {
            PhotoAsset* asset = assetByExactKey.value(key, nullptr);
            if (asset != nullptr) {
                stems.append(asset->stem);
            }
        }
        const QString message =
            tr("Stems '%1' differ only by letter case or Unicode normalisation. They are kept "
               "separate; resolve the intended grouping before comparing or operating on them.")
                .arg(stems.join(QStringLiteral("', '")));
        for (const QString& key : exactKeys) {
            PhotoAsset* asset = assetByExactKey.value(key, nullptr);
            if (asset == nullptr) {
                continue;
            }
            asset->pairingState = PairingState::Ambiguous;
            asset->operationsBlocked = true;
            asset->diagnostics.append(message);
        }
        result.diagnostics.append(message);
    }

    // ---- 6. Deterministic natural order -------------------------------------
    std::stable_sort(result.assets.begin(), result.assets.end(),
                     [](const PhotoAsset& lhs, const PhotoAsset& rhs) {
                         const int directory =
                             naturalCompare(lhs.relativeDirectory, rhs.relativeDirectory);
                         if (directory != 0) {
                             return directory < 0;
                         }
                         return naturalCompare(lhs.stem, rhs.stem) < 0;
                     });

    // Membership revision derives from the resolved member set, so an unchanged
    // rescan leaves frozen selections valid.
    for (PhotoAsset& asset : result.assets) {
        QCryptographicHash hash(QCryptographicHash::Sha1);
        for (const FileMember& member : asset.members) {
            hash.addData(member.absolutePath.toUtf8());
            hash.addData(QByteArrayLiteral("\x1f"));
        }
        const QByteArray digest = hash.result();
        quint64 revision = 0;
        for (int i = 0; i < 8 && i < digest.size(); ++i) {
            // Widen to the unsigned accumulator before the bitwise OR: a
            // quint8 promotes to int, which would mix signedness.
            revision = (revision << 8U) | static_cast<quint64>(static_cast<quint8>(digest.at(i)));
        }
        asset.membershipRevision = revision;
    }

    return result;
}

} // namespace cullfinch::domain
