// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/Services.h>

#include <algorithm>

namespace cullfinch::application {
namespace {

/// The step every member of one group is at, or an empty string when they
/// disagree.
///
/// A group is the unit an offer applies to: its files move together, reach
/// Trash together and come back together, so a group whose members are at
/// different steps is precisely the half-accounted-for case that no offer
/// covers.
QString groupStep(const OperationRecord& record, const domain::PlannedGroup& group) {
    QString common;
    bool first = true;
    for (const domain::PlannedMember& member : group.members) {
        const auto entry =
            std::ranges::find(record.members, member.memberId, &OperationMemberRecord::memberId);
        if (entry == record.members.cend()) {
            // The record does not know this member, so it cannot speak for the
            // group either.
            return {};
        }
        if (first) {
            common = entry->lastDurableStep;
            first = false;
        } else if (common != entry->lastDurableStep) {
            return {};
        }
    }
    return common;
}

} // namespace

QString sessionLifecycleToken(SessionLifecycle lifecycle) {
    using enum SessionLifecycle;
    switch (lifecycle) {
    case Active:
        return QStringLiteral("active");
    case Paused:
        return QStringLiteral("paused");
    case Finished:
        return QStringLiteral("finished");
    case Discarded:
        break;
    }
    return QStringLiteral("discarded");
}

SessionLifecycle sessionLifecycleFromToken(const QString& token) {
    using enum SessionLifecycle;
    if (token == QLatin1String("active")) {
        return Active;
    }
    if (token == QLatin1String("paused")) {
        return Paused;
    }
    if (token == QLatin1String("finished")) {
        return Finished;
    }
    return Discarded;
}

RecoveryOffers recoveryOffersFor(const OperationRecord& record) {
    RecoveryOffers offers;
    if (record.state == domain::OperationState::Completed || record.plan.groups.isEmpty()) {
        return offers;
    }

    int trashed = 0;
    int staged = 0;
    int uncertain = 0;
    for (const domain::PlannedGroup& group : record.plan.groups) {
        const QString step = groupStep(record, group);
        if (step == operationStep::trashed) {
            ++trashed;
        } else if (step == operationStep::staged) {
            ++staged;
        } else if (step == operationStep::uncertain) {
            ++uncertain;
        }
    }

    // A complete group in staging can be put back where it came from. Worth
    // offering even for part of a plan: the rest is already accounted for, and
    // a restore never overwrites.
    offers.restore = staged > 0;
    // Trash is asked again only when every group is either already there or
    // intact in staging. One left at its original paths means the plan no
    // longer describes the work, so that is a restore-and-review job.
    offers.retryTrash = staged > 0 && staged + trashed == record.plan.groups.size();
    // A person who has checked the Trash can settle what the filesystem cannot
    // show -- but only once nothing else about the operation is still in the
    // air, or "confirmed" would be answering for groups nobody looked at.
    offers.confirmTrashed = uncertain > 0 && uncertain + trashed == record.plan.groups.size();
    return offers;
}

bool needsRecoveryAttention(const OperationRecord& record) {
    using enum domain::OperationState;
    switch (record.state) {
    case Completed:
    case Failed:
    case Planned:
        // Done, or stopped having left nothing on the source filesystem, or --
        // for Planned -- either not started or already put back by a recovery.
        // All three leave the collection exactly as it was, so none of them is
        // a repair job. Marks that were never acted on are still marks, and
        // reviewing file operations again is the way to act on them.
        return false;
    case Staging:
    case Staged:
    case Trashing:
    case Restoring:
    case NeedsRecovery:
        break;
    }
    return true;
}

} // namespace cullfinch::application
