// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FileMember.h>
#include <cullfinch/domain/Ids.h>

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace cullfinch::domain {

/// Whether a group of files can safely take part in comparison and operations.
///
/// Deliberately independent of Disposition and of operation lifecycle state:
/// combining them into one large status enum is what the design forbids.
enum class PairingState {
    /// The association scope has not finished enumerating for this scan
    /// generation. A provisional group cannot start a comparison or an
    /// operation, because a JPG seen before its RAW would otherwise acquire
    /// incorrect membership.
    Provisional,
    Resolved,  ///< One JPG preview plus one or more RAW companions.
    JpegOnly,  ///< One JPG, no companion found in the association scope.
    RawOnly,   ///< RAW files with no JPG. A diagnostic entry, never rendered.
    Ambiguous, ///< Duplicate or conflicting previews; needs user resolution.
    Stale      ///< A previously known member no longer matches the filesystem.
};

[[nodiscard]] QString pairingStateName(PairingState state);

/// A collection-level deletion mark. Intent only; physical work is separate.
enum class Disposition { Neutral, Reject };

/// One visible photo and all of its known associated files.
///
/// This, not an individual file, is the unit of selection, rejection and file
/// operations.
struct PhotoAsset {
    AssetId id;
    CollectionId collectionId;

    QString stem;              ///< Exact filename stem as it appears on disk.
    QString relativeDirectory; ///< Relative to the scan root; empty at the root.
    QString displayName;

    MemberId previewMemberId;
    FileMemberList members;

    PairingState pairingState = PairingState::Provisional;
    Disposition disposition = Disposition::Neutral;

    /// Bumped whenever membership changes, so a frozen selection can detect
    /// that the group underneath it moved.
    quint64 membershipRevision = 0;

    /// Set when something about the group makes physical file operations
    /// unsafe even though it is perfectly fine to look at: a symlink, a
    /// hardlink alias, or an unclassified same-stem file. Kept separate from
    /// pairingState so browsing is not blocked by an operation-only concern.
    bool operationsBlocked = false;
    QStringList diagnostics;

    /// True when the asset has a real JPG preview to display.
    [[nodiscard]] bool isComparable() const;

    /// True when the asset may enter a physical file-operation plan.
    [[nodiscard]] bool isOperable() const;

    [[nodiscard]] const FileMember* preview() const;
    [[nodiscard]] const FileMember* memberById(const MemberId& id) const;
    [[nodiscard]] FileMemberList membersWithRole(MemberRole role) const;
    [[nodiscard]] int rawCount() const;
    [[nodiscard]] qint64 totalBytes() const;
};

using PhotoAssetList = QList<PhotoAsset>;

} // namespace cullfinch::domain

Q_DECLARE_METATYPE(cullfinch::domain::PhotoAsset)
