// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FileMember.h>
#include <cullfinch/domain/Ids.h>
#include <cullfinch/domain/PhotoAsset.h>

#include <QList>
#include <QString>

namespace cullfinch::domain {

enum class OperationKind { MoveToTrash };

/// Explicit lifecycle states. Each retry revalidates its preconditions and is
/// safe to repeat for the same journal state.
enum class OperationState {
    Planned,
    Staging,
    Staged,
    Trashing,
    Completed,
    Restoring,
    Failed,
    NeedsRecovery
};

[[nodiscard]] QString operationStateName(OperationState state);
[[nodiscard]] QString operationStateToken(OperationState state);
[[nodiscard]] OperationState operationStateFromToken(const QString& token);

/// Per-file work inside a group. `expected` is the identity recorded at
/// planning time; execution refuses to move a file that no longer matches it.
struct PlannedMember {
    MemberId memberId;
    MemberRole role = MemberRole::Unknown;
    QString sourcePath;
    QString fileName;
    FileFingerprint expected;
};

/// A whole photo. An operation is always planned for the entire asset,
/// including every resolved member (invariant 5).
struct PlannedGroup {
    AssetId assetId;
    QString displayName;
    QString stagingDirectoryName; ///< Unique within the staging root.
    QList<PlannedMember> members;

    [[nodiscard]] qint64 totalBytes() const;
    [[nodiscard]] bool containsMember(const MemberId& id) const;
};

/// An immutable, reviewed unit of file-operation work.
struct OperationPlan {
    OperationId id;
    CollectionId collectionId;
    quint64 collectionRevision = 0;
    OperationKind kind = OperationKind::MoveToTrash;
    QString stagingRoot;
    QList<PlannedGroup> groups;

    [[nodiscard]] int logicalPhotoCount() const { return static_cast<int>(groups.size()); }
    [[nodiscard]] int physicalFileCount() const;
    [[nodiscard]] qint64 totalBytes() const;
    [[nodiscard]] bool isEmpty() const { return groups.isEmpty(); }
};

struct PlanningIssue {
    AssetId assetId;
    QString displayName;
    QString reason;
};

struct PlanningResult {
    OperationPlan plan;
    QList<PlanningIssue> blocked;

    [[nodiscard]] bool hasBlockers() const { return !blocked.isEmpty(); }
};

/// Turns rejected assets into reviewable, immutable groups.
///
/// Pure: it inspects the supplied asset snapshots and never touches the
/// filesystem. Re-enumeration and identity verification happen immediately
/// before execution, in the infrastructure layer.
class OperationPlanner {
public:
    /// @param stagingRoot collection-local staging directory, excluded from
    ///        scanning. Must live on the same filesystem as the sources.
    [[nodiscard]] static PlanningResult plan(const CollectionId& collectionId,
                                             quint64 collectionRevision, const QString& stagingRoot,
                                             const PhotoAssetList& rejectedAssets);
};

} // namespace cullfinch::domain
