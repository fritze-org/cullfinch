// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/OperationPlan.h>

#include <QCoreApplication>
#include <QHash>

#include <algorithm>

namespace cullfinch::domain {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

} // namespace

QString operationStateName(OperationState state) {
    using enum OperationState;
    switch (state) {
    case Planned:
        return tr("Planned");
    case Staging:
        return tr("Staging");
    case Staged:
        return tr("Staged");
    case Trashing:
        return tr("Moving to Trash");
    case Completed:
        return tr("Completed");
    case Restoring:
        return tr("Restoring");
    case Failed:
        return tr("Failed");
    case NeedsRecovery:
        break;
    }
    return tr("Needs recovery");
}

QString operationStateToken(OperationState state) {
    using enum OperationState;
    switch (state) {
    case Planned:
        return QStringLiteral("planned");
    case Staging:
        return QStringLiteral("staging");
    case Staged:
        return QStringLiteral("staged");
    case Trashing:
        return QStringLiteral("trashing");
    case Completed:
        return QStringLiteral("completed");
    case Restoring:
        return QStringLiteral("restoring");
    case Failed:
        return QStringLiteral("failed");
    case NeedsRecovery:
        break;
    }
    return QStringLiteral("needs-recovery");
}

OperationState operationStateFromToken(const QString& token) {
    using enum OperationState;
    static const QHash<QString, OperationState> states = {
        {QStringLiteral("planned"), Planned},
        {QStringLiteral("staging"), Staging},
        {QStringLiteral("staged"), Staged},
        {QStringLiteral("trashing"), Trashing},
        {QStringLiteral("completed"), Completed},
        {QStringLiteral("restoring"), Restoring},
        {QStringLiteral("failed"), Failed},
        {QStringLiteral("needs-recovery"), NeedsRecovery}};
    // An unreadable token is treated as needing recovery: never as completed.
    return states.value(token, NeedsRecovery);
}

qint64 PlannedGroup::totalBytes() const {
    qint64 total = 0;
    for (const PlannedMember& member : members) {
        if (member.expected.sizeBytes > 0) {
            total += member.expected.sizeBytes;
        }
    }
    return total;
}

bool PlannedGroup::containsMember(const MemberId& id) const {
    return std::ranges::any_of(
        members, [&id](const PlannedMember& member) { return member.memberId == id; });
}

int OperationPlan::physicalFileCount() const {
    int count = 0;
    for (const PlannedGroup& group : groups) {
        count += static_cast<int>(group.members.size());
    }
    return count;
}

qint64 OperationPlan::totalBytes() const {
    qint64 total = 0;
    for (const PlannedGroup& group : groups) {
        total += group.totalBytes();
    }
    return total;
}

PlanningResult OperationPlanner::plan(const CollectionId& collectionId, quint64 collectionRevision,
                                      const QString& stagingRoot,
                                      const PhotoAssetList& rejectedAssets) {
    PlanningResult result;
    result.plan.id = OperationId::generate();
    result.plan.collectionId = collectionId;
    result.plan.collectionRevision = collectionRevision;
    result.plan.stagingRoot = stagingRoot;

    // A file member belongs to at most one resolved asset (invariant 7). If the
    // same physical file turns up in two groups, both are blocked rather than
    // silently deduplicated.
    QHash<QString, AssetId> pathOwner;
    QHash<AssetId, QString> conflictReason;

    PhotoAssetList candidates;
    for (const PhotoAsset& asset : rejectedAssets) {
        if (asset.disposition != Disposition::Reject) {
            result.blocked.append(PlanningIssue{
                asset.id, asset.displayName,
                tr("Not marked for deletion; only marked photos enter a file operation.")});
            continue;
        }
        if (!asset.isOperable()) {
            QString reason = tr("The file group must be resolved before any file is moved.");
            if (asset.operationsBlocked && !asset.diagnostics.isEmpty()) {
                reason = asset.diagnostics.first();
            } else if (asset.pairingState == PairingState::Stale) {
                reason = tr("The files changed on disk after they were marked.");
            } else if (asset.pairingState == PairingState::Provisional) {
                reason = tr("Still being scanned.");
            }
            result.blocked.append(PlanningIssue{asset.id, asset.displayName, reason});
            continue;
        }
        if (asset.members.isEmpty()) {
            result.blocked.append(
                PlanningIssue{asset.id, asset.displayName, tr("No known files to move.")});
            continue;
        }

        for (const FileMember& member : asset.members) {
            const auto owner = pathOwner.constFind(member.absolutePath);
            if (owner != pathOwner.constEnd() && !(*owner == asset.id)) {
                const QString reason =
                    tr("'%1' is claimed by more than one photo. Resolve the association first.")
                        .arg(member.fileName);
                conflictReason.insert(asset.id, reason);
                conflictReason.insert(*owner, reason);
            } else {
                pathOwner.insert(member.absolutePath, asset.id);
            }
        }
        candidates.append(asset);
    }

    for (const PhotoAsset& asset : candidates) {
        if (const auto conflict = conflictReason.constFind(asset.id);
            conflict != conflictReason.constEnd()) {
            result.blocked.append(PlanningIssue{asset.id, asset.displayName, *conflict});
            continue;
        }

        PlannedGroup group;
        group.assetId = asset.id;
        group.displayName = asset.displayName;
        // Unique per asset and per plan, so the operation's own new directories
        // never collide with an existing entry.
        group.stagingDirectoryName = QStringLiteral("%1-%2").arg(result.plan.id.toString().left(8),
                                                                 asset.id.toString().left(16));

        for (const FileMember& member : asset.members) {
            group.members.append(PlannedMember{member.id, member.role, member.absolutePath,
                                               member.fileName, member.fingerprint});
        }
        result.plan.groups.append(group);
    }

    return result;
}

} // namespace cullfinch::domain
