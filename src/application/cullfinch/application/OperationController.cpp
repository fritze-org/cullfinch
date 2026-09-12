// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/OperationController.h>

#include <QCoreApplication>
#include <QDateTime>

#include <utility>

namespace cullfinch::application {
namespace {

/// The journal record a plan starts from: one entry per physical file, every one
/// of them still at the planned step because nothing has moved yet.
OperationRecord plannedRecord(const domain::OperationPlan& plan) {
    OperationRecord record;
    record.plan = plan;
    record.state = domain::OperationState::Planned;
    record.createdUtc = QDateTime::currentDateTimeUtc();
    record.updatedUtc = record.createdUtc;
    for (const domain::PlannedGroup& group : plan.groups) {
        for (const domain::PlannedMember& member : group.members) {
            OperationMemberRecord entry;
            entry.memberId = member.memberId;
            entry.assetId = group.assetId;
            entry.sourcePath = member.sourcePath;
            entry.lastDurableStep = operationStep::planned;
            record.members.append(entry);
        }
    }
    return record;
}

/// True when the run stopped with work still on the source filesystem, which is
/// the only condition recovery can act on.
bool stoppedWithRecoverableWork(const OperationRecord& record) {
    return record.state == domain::OperationState::Failed ||
           record.state == domain::OperationState::NeedsRecovery;
}

} // namespace

OperationController::OperationController(IAssetRepository& repository, IOperationExecutor& executor,
                                         QObject* parent)
    : QObject(parent), repository_(repository), executor_(executor) {}

domain::PlanningResult OperationController::review(const domain::CollectionId& collectionId,
                                                   quint64 collectionRevision,
                                                   const QString& stagingRoot,
                                                   const domain::PhotoAssetList& rejected) const {
    return domain::OperationPlanner::plan(collectionId, collectionRevision, stagingRoot, rejected);
}

bool OperationController::persist(const OperationRecord& record, QString* error) {
    if (QString storageError; !repository_.saveOperation(record, &storageError)) {
        if (error != nullptr) {
            *error = tr("The operation journal could not be written: %1").arg(storageError);
        }
        return false;
    }
    return true;
}

bool OperationController::verifyAgainstDisk(const domain::OperationPlan& plan,
                                            const domain::PhotoAssetList& current, QString* error) {
    QString preflightError;
    const domain::PlanningResult verified = executor_.preflight(plan, current, &preflightError);
    if (!preflightError.isEmpty()) {
        if (error != nullptr) {
            *error = std::move(preflightError);
        }
        return false;
    }
    if (!verified.hasBlockers()) {
        return true;
    }
    Q_EMIT planInvalidated(verified);
    if (error != nullptr) {
        *error = tr("The files changed since this operation was reviewed. Review it again.");
    }
    return false;
}

bool OperationController::stageGroups(const domain::OperationPlan& plan, OperationRecord& record,
                                      const JournalWriter& journal, QString* error) {
    int completed = 0;
    const int total = plan.logicalPhotoCount();
    Q_EMIT progressChanged(completed, total);

    for (const domain::PlannedGroup& group : plan.groups) {
        record = executor_.executeGroup(record, group, journal);
        record.updatedUtc = QDateTime::currentDateTimeUtc();

        if (QString storageError; !persist(record, &storageError)) {
            // The journal is the only thing that makes staged work recoverable,
            // so a failure to record it stops the run.
            Q_EMIT errorOccurred(storageError);
            if (error != nullptr) {
                *error = std::move(storageError);
            }
            return false;
        }
        Q_EMIT recordChanged(record);

        if (stoppedWithRecoverableWork(record)) {
            if (error != nullptr) {
                *error = record.error.isEmpty()
                             ? tr("The operation stopped and left recoverable work.")
                             : record.error;
            }
            return false;
        }

        ++completed;
        Q_EMIT progressChanged(completed, total);
    }
    return true;
}

bool OperationController::execute(const domain::OperationPlan& plan,
                                  const domain::PhotoAssetList& current, QString* error) {
    if (plan.isEmpty()) {
        if (error != nullptr) {
            *error = tr("There is nothing to move.");
        }
        return false;
    }

    // Re-enumerate and verify immediately before execution.
    if (!verifyAgainstDisk(plan, current, error)) {
        return false;
    }

    OperationRecord record = plannedRecord(plan);

    // The plan is persisted before anything moves.
    if (!persist(record, error)) {
        return false;
    }
    Q_EMIT recordChanged(record);

    // Every rename is bracketed by a journal write: the intent before the
    // move, the outcome after it. A crash between the two leaves a record
    // that names the intended destination, which is what recovery inspects.
    const application::JournalWriter journal = [this](OperationRecord snapshot,
                                                      QString* journalError) {
        snapshot.updatedUtc = QDateTime::currentDateTimeUtc();
        if (!persist(snapshot, journalError)) {
            return false;
        }
        Q_EMIT recordChanged(snapshot);
        return true;
    };

    if (!stageGroups(plan, record, journal, error)) {
        return false;
    }

    record.state = domain::OperationState::Completed;
    record.updatedUtc = QDateTime::currentDateTimeUtc();
    if (!persist(record, error)) {
        return false;
    }
    Q_EMIT recordChanged(record);
    return true;
}

QList<OperationRecord>
OperationController::unfinished(const domain::CollectionId& collectionId) const {
    QString error;
    return repository_.unfinishedOperations(collectionId, &error);
}

QList<OperationRecord>
OperationController::needingRecovery(const domain::CollectionId& collectionId) const {
    QList<OperationRecord> result;
    for (const OperationRecord& record : unfinished(collectionId)) {
        if (needsRecoveryAttention(record)) {
            result.append(record);
        }
    }
    return result;
}

bool OperationController::applyRecovery(
    const domain::OperationId& operationId,
    OperationRecord (IOperationExecutor::*apply)(const OperationRecord&), QString* error) {
    QString storageError;
    const std::optional<OperationRecord> stored =
        repository_.loadOperation(operationId, &storageError);
    if (!stored.has_value()) {
        if (error != nullptr) {
            *error = tr("That operation is not recorded: %1").arg(storageError);
        }
        return false;
    }

    OperationRecord recovered = (executor_.*apply)(*stored);
    recovered.updatedUtc = QDateTime::currentDateTimeUtc();
    // Journalled before anyone is told: what the executor found is worth no
    // less than what it did, and a second attempt must start from it.
    if (!persist(recovered, error)) {
        return false;
    }
    Q_EMIT recordChanged(recovered);

    if (stoppedWithRecoverableWork(recovered)) {
        if (error != nullptr) {
            *error = recovered.error.isEmpty() ? tr("The operation still needs attention.")
                                               : recovered.error;
        }
        return false;
    }
    return true;
}

bool OperationController::recover(const domain::OperationId& operationId, QString* error) {
    return applyRecovery(operationId, &IOperationExecutor::recover, error);
}

bool OperationController::retryTrash(const domain::OperationId& operationId, QString* error) {
    return applyRecovery(operationId, &IOperationExecutor::retryTrash, error);
}

bool OperationController::confirmTrashed(const domain::OperationId& operationId, QString* error) {
    return applyRecovery(operationId, &IOperationExecutor::confirmTrashed, error);
}

} // namespace cullfinch::application
