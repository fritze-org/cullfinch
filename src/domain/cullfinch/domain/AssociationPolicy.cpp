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
           QLatin1Char('\x{1f}') + stem.normalized(QString::NormalizationForm_C).toCaseFolded();
}

QString exactKey(const QString& relativeDirectory, const QString& stem) {
    return relativeDirectory + QLatin1Char('\x{1f}') + stem;
}

int roleRank(MemberRole role) {
    using enum MemberRole;
    switch (role) {
    case Jpeg:
        return 0;
    case Raw:
        return 1;
    case Sidecar:
        return 2;
    case Unknown:
        break;
    }
    return 3;
}

/// What a group's members add up to. Pairing state is decided from these counts
/// alone, so counting and classifying stay separable.
struct MemberTally {
    int jpegCount = 0;
    int rawCount = 0;
    int unknownCount = 0;
    MemberId firstJpegId;
};

/// Bucket the discovered files by their exact (directory, stem) key.
///
/// `foldedToExact` accumulates, per case-folded and normalised key, every exact
/// key that collapses onto it. More than one means a collision to report.
void bucketByStem(const QList<DiscoveredFile>& files, const AssociationConfig& config,
                  QMap<QString, QList<DiscoveredFile>>& groups,
                  QHash<QString, QStringList>& foldedToExact) {
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
}

/// Paths that are a hardlink alias of another discovered path, mapped to true.
///
/// Aliases are found across the whole scan rather than per group, because the
/// two names for one inode need not share a stem or a directory.
QHash<QString, bool> findHardlinkAliases(const QList<DiscoveredFile>& files,
                                         QStringList& diagnostics) {
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
            diagnostics.append(tr("Hardlink alias: '%1' and '%2' are the same file.")
                                   .arg(*existing, file.absolutePath));
        } else {
            nativeIdentityOwner.insert(identity, file.absolutePath);
        }
    }
    return aliasedPath;
}

FileMember makeMember(const DiscoveredFile& entry, const AssociationConfig& config) {
    FileMember member;
    member.id = StemAssociationResolver::deriveMemberId(entry.absolutePath);
    member.role = config.roleFor(entry.extension);
    member.absolutePath = entry.absolutePath;
    member.fileName = entry.fileName;
    member.extensionLower = entry.extension.toCaseFolded();
    member.fingerprint = entry.fingerprint;
    member.isSymlink = entry.isSymlink;
    return member;
}

/// Add one member to the asset, counting its role and recording every reason it
/// has to block file operations.
void absorbMember(const FileMember& member, bool aliased, PhotoAsset& asset, MemberTally& tally) {
    using enum MemberRole;
    switch (member.role) {
    case Jpeg:
        ++tally.jpegCount;
        if (!tally.firstJpegId.isValid()) {
            tally.firstJpegId = member.id;
        }
        break;
    case Raw:
        ++tally.rawCount;
        break;
    case Unknown:
        ++tally.unknownCount;
        asset.diagnostics.append(
            tr("Unclassified file with the same stem: '%1'. Classify it before file "
               "operations on this photo.")
                .arg(member.fileName));
        // Blocks operations, not browsing.
        asset.operationsBlocked = true;
        break;
    case Sidecar:
        break;
    }

    if (member.isSymlink) {
        asset.diagnostics.append(
            tr("'%1' is a symbolic link. Symlinks are not followed for file operations.")
                .arg(member.fileName));
        asset.operationsBlocked = true;
    }
    if (aliased) {
        asset.diagnostics.append(
            tr("'%1' is a hardlink alias of another discovered file.").arg(member.fileName));
        asset.operationsBlocked = true;
    }

    asset.members.append(member);
}

void classifyPairing(const MemberTally& tally, PhotoAsset& asset) {
    using enum PairingState;
    if (tally.jpegCount > 1) {
        asset.pairingState = Ambiguous;
        asset.diagnostics.append(
            tr("%1 candidate previews share this stem. Resolve which one represents the "
               "photo before comparing or operating on it.")
                .arg(tally.jpegCount));
    } else if (tally.jpegCount == 1) {
        asset.previewMemberId = tally.firstJpegId;
        asset.pairingState = tally.rawCount > 0 ? Resolved : JpegOnly;
    } else if (tally.rawCount > 0) {
        asset.pairingState = RawOnly;
        asset.diagnostics.append(
            tr("No JPG preview accompanies this RAW file. Cullfinch does not render RAW "
               "embedded previews."));
    } else {
        asset.pairingState = Ambiguous;
        asset.diagnostics.append(tr("No recognised image file with this stem."));
    }

    if (tally.unknownCount > 0 && asset.pairingState == Resolved) {
        // Membership is still resolved; only operations are held back.
        asset.diagnostics.append(tr("Comparison is unaffected; file operations are blocked."));
    }
}

/// One asset from the files sharing an exact (directory, stem) key.
PhotoAsset buildAsset(const CollectionId& collectionId, const QList<DiscoveredFile>& entries,
                      const AssociationConfig& config, const QHash<QString, bool>& aliasedPath,
                      bool scopeComplete) {
    const DiscoveredFile& first = entries.first();

    PhotoAsset asset;
    asset.collectionId = collectionId;
    asset.id =
        StemAssociationResolver::deriveAssetId(collectionId, first.relativeDirectory, first.stem);
    asset.stem = first.stem;
    asset.relativeDirectory = first.relativeDirectory;
    asset.displayName = first.relativeDirectory.isEmpty()
                            ? first.stem
                            : first.relativeDirectory + QLatin1Char('/') + first.stem;

    MemberTally tally;
    for (const DiscoveredFile& entry : entries) {
        absorbMember(makeMember(entry, config), aliasedPath.value(entry.absolutePath, false), asset,
                     tally);
    }

    // Preview first, then RAW companions, then sidecars, then unclassified.
    std::ranges::stable_sort(asset.members, [](const FileMember& lhs, const FileMember& rhs) {
        if (roleRank(lhs.role) != roleRank(rhs.role)) {
            return roleRank(lhs.role) < roleRank(rhs.role);
        }
        return naturalLess(lhs.fileName, rhs.fileName);
    });

    classifyPairing(tally, asset);

    // A group whose association scope is still being enumerated must not be
    // classified yet, nor take part in comparisons or operations.
    if (!scopeComplete) {
        asset.pairingState = PairingState::Provisional;
    }

    return asset;
}

/// Flag every asset whose stem only differs from another's by case or Unicode
/// normalisation. They stay separate assets; the collision is reported, never
/// resolved by rewriting a filename.
void flagFoldedCollisions(const QHash<QString, QStringList>& foldedToExact,
                          AssociationResult& result) {
    QHash<QString, PhotoAsset*> assetByExactKey;
    for (PhotoAsset& asset : result.assets) {
        assetByExactKey.insert(exactKey(asset.relativeDirectory, asset.stem), &asset);
    }

    for (const QStringList& exactKeys : foldedToExact) {
        if (exactKeys.size() < 2) {
            continue;
        }
        QStringList stems;
        for (const QString& key : exactKeys) {
            if (const PhotoAsset* asset = assetByExactKey.value(key, nullptr); asset != nullptr) {
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
}

/// Membership revision derives from the resolved member set, so an unchanged
/// rescan leaves frozen selections valid.
void assignMembershipRevisions(PhotoAssetList& assets) {
    for (PhotoAsset& asset : assets) {
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
    using enum MemberRole;
    const QString folded = extension.toCaseFolded();
    if (jpegExtensions.contains(folded)) {
        return Jpeg;
    }
    if (rawExtensions.contains(folded)) {
        return Raw;
    }
    if (sidecarsEnabled && sidecarExtensions.contains(folded)) {
        return Sidecar;
    }
    return Unknown;
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

    // QMap keeps a deterministic key order; the final asset order is decided by
    // natural filename comparison at the end.
    QMap<QString, QList<DiscoveredFile>> groups;
    QHash<QString, QStringList> foldedToExact;
    bucketByStem(files, config, groups, foldedToExact);

    const QHash<QString, bool> aliasedPath = findHardlinkAliases(files, result.diagnostics);

    result.assets.reserve(groups.size());
    for (const QList<DiscoveredFile>& entries : groups) {
        result.assets.append(buildAsset(collectionId, entries, config, aliasedPath, scopeComplete));
    }

    flagFoldedCollisions(foldedToExact, result);

    std::ranges::stable_sort(result.assets, [](const PhotoAsset& lhs, const PhotoAsset& rhs) {
        if (const int directory = naturalCompare(lhs.relativeDirectory, rhs.relativeDirectory);
            directory != 0) {
            return directory < 0;
        }
        return naturalCompare(lhs.stem, rhs.stem) < 0;
    });

    assignMembershipRevisions(result.assets);

    return result;
}

} // namespace cullfinch::domain
