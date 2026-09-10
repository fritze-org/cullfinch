// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/OperationController.h>

#include <QCoreApplication>
#include <QDateTime>

namespace cullfinch::application {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
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
    QString storageError;
    if (!repository_.saveOperation(record, &storageError)) {
        if (error != nullptr) {
            *error = tr("The operation journal could not be written: %1").arg(storageError);
        }
        return false;
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
    QString preflightError;
    const domain::PlanningResult verified = executor_.preflight(plan, current, &preflightError);
    if (!preflightError.isEmpty()) {
        if (error != nullptr) {
            *error = preflightError;
        }
        return false;
    }
    if (verified.hasBlockers()) {
        Q_EMIT planInvalidated(verified);
        if (error != nullptr) {
            *error = tr("The files changed since this operation was reviewed. Review it again.");
        }
        return false;
    }

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
            entry.lastDurableStep = QStringLiteral("planned");
            record.members.append(entry);
        }
    }

    // The plan is persisted before anything moves.
    if (!persist(record, error)) {
        return false;
    }
    Q_EMIT recordChanged(record);

    int completed = 0;
    const int total = plan.logicalPhotoCount();
    Q_EMIT progressChanged(completed, total);

    for (const domain::PlannedGroup& group : plan.groups) {
        record = executor_.executeGroup(record, group);
        record.updatedUtc = QDateTime::currentDateTimeUtc();

        QString storageError;
        if (!persist(record, &storageError)) {
            // The journal is the only thing that makes staged work recoverable,
            // so a failure to record it stops the run.
            Q_EMIT errorOccurred(storageError);
            if (error != nullptr) {
                *error = storageError;
            }
            return false;
        }
        Q_EMIT recordChanged(record);

        if (record.state == domain::OperationState::Failed ||
            record.state == domain::OperationState::NeedsRecovery) {
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

bool OperationController::recover(const domain::OperationId& operationId, QString* error) {
    QString storageError;
    const std::optional<OperationRecord> stored =
        repository_.loadOperation(operationId, &storageError);
    if (!stored.has_value()) {
        if (error != nullptr) {
            *error = tr("That operation is not recorded: %1").arg(storageError);
        }
        return false;
    }

    OperationRecord recovered = executor_.recover(*stored);
    recovered.updatedUtc = QDateTime::currentDateTimeUtc();
    if (!persist(recovered, error)) {
        return false;
    }
    Q_EMIT recordChanged(recovered);

    if (recovered.state == domain::OperationState::Failed ||
        recovered.state == domain::OperationState::NeedsRecovery) {
        if (error != nullptr) {
            *error = recovered.error.isEmpty() ? tr("The operation still needs attention.")
                                               : recovered.error;
        }
        return false;
    }
    return true;
}

} // namespace cullfinch::application
