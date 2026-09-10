// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FileMember.h>
#include <cullfinch/domain/Ids.h>
#include <cullfinch/domain/PhotoAsset.h>

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace cullfinch::domain {

/// One file as reported by a scan, before any pairing decision is made.
struct DiscoveredFile {
    QString absolutePath;
    QString relativeDirectory; ///< Relative to the scan root; empty at the root.
    QString fileName;
    QString stem;      ///< Exact stem as on disk, i.e. name without the last suffix.
    QString extension; ///< Exact extension as on disk, without the leading dot.
    FileFingerprint fingerprint;
    bool isSymlink = false;
};

/// Which extensions are recognised, and as what.
///
/// The RAW list recognises companion *candidates*. It makes no claim that the
/// contents of those files are supported for decoding: cullfinch never decodes
/// RAW bytes.
struct AssociationConfig {
    QStringList jpegExtensions;
    QStringList rawExtensions;
    QStringList sidecarExtensions;
    bool sidecarsEnabled = false;

    [[nodiscard]] static AssociationConfig defaults();

    /// Role for an extension, case-insensitively. Sidecar extensions resolve to
    /// Sidecar only while sidecars are enabled.
    [[nodiscard]] MemberRole roleFor(const QString& extension) const;

    /// True when the extension is a sidecar candidate that is currently
    /// switched off, and therefore ignored rather than treated as unclassified.
    [[nodiscard]] bool isDisabledSidecar(const QString& extension) const;
};

struct AssociationResult {
    PhotoAssetList assets;
    QStringList diagnostics;
};

/// Pairing is a policy, not a hardcoded rule (section 4.3). Later resolvers can
/// map corresponding JPG/RAW directory trees or filename transformations
/// without touching the flow, browser or operation code.
class IAssociationResolver {
public:
    IAssociationResolver() = default;
    virtual ~IAssociationResolver() = default;
    IAssociationResolver(const IAssociationResolver&) = delete;
    IAssociationResolver& operator=(const IAssociationResolver&) = delete;
    IAssociationResolver(IAssociationResolver&&) = delete;
    IAssociationResolver& operator=(IAssociationResolver&&) = delete;

    [[nodiscard]] virtual QString id() const = 0;
    [[nodiscard]] virtual QString displayName() const = 0;

    /// Group discovered files into photo assets.
    ///
    /// @param scopeComplete false while the association scope is still being
    ///        enumerated for the current scan generation. Every produced asset
    ///        is then Provisional, because a JPG encountered before its RAW
    ///        would otherwise acquire incorrect membership.
    [[nodiscard]] virtual AssociationResult resolve(const CollectionId& collectionId,
                                                    const QList<DiscoveredFile>& files,
                                                    const AssociationConfig& config,
                                                    bool scopeComplete) const = 0;
};

/// The confirmed rule: pair within the same directory using the exact filename
/// stem, recognising extensions case-insensitively.
///
/// Case-folding and Unicode-normalisation collisions are detected and reported;
/// they never cause filenames to be rewritten or assets to be merged.
class StemAssociationResolver final : public IAssociationResolver {
public:
    [[nodiscard]] QString id() const override;
    [[nodiscard]] QString displayName() const override;

    [[nodiscard]] AssociationResult resolve(const CollectionId& collectionId,
                                            const QList<DiscoveredFile>& files,
                                            const AssociationConfig& config,
                                            bool scopeComplete) const override;

    /// Deterministic identity for a group, so a rescan reconciles onto the same
    /// stored asset instead of inventing a new one.
    [[nodiscard]] static AssetId deriveAssetId(const CollectionId& collectionId,
                                               const QString& relativeDirectory,
                                               const QString& stem);

    [[nodiscard]] static MemberId deriveMemberId(const QString& absolutePath);
};

} // namespace cullfinch::domain

Q_DECLARE_METATYPE(cullfinch::domain::AssociationResult)
Q_DECLARE_METATYPE(cullfinch::domain::AssociationConfig)
