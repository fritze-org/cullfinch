// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>
#include <cullfinch/domain/OperationPlan.h>

#include <QList>
#include <QObject>
#include <QString>

namespace cullfinch::application {

/// Owns review plans, execution and recovery.
///
/// Physical file operations are deliberately not QUndoCommands: starting a
/// reviewed operation establishes a collection undo boundary, and restoration
/// is a separate explicit operation.
class OperationController : public QObject {
    Q_OBJECT

public:
    OperationController(IAssetRepository& repository, IOperationExecutor& executor,
                        QObject* parent = nullptr);

    /// Build the immutable plan the review screen shows.
    [[nodiscard]] domain::PlanningResult review(const domain::CollectionId& collectionId,
                                                quint64 collectionRevision,
                                                const QString& stagingRoot,
                                                const domain::PhotoAssetList& rejected) const;

    /// Re-verify and then execute a reviewed plan.
    ///
    /// If the filesystem no longer matches what was reviewed, the plan is
    /// invalidated and the caller must return to review.
    bool execute(const domain::OperationPlan& plan, const domain::PhotoAssetList& current,
                 QString* error);

    [[nodiscard]] QList<OperationRecord> unfinished(const domain::CollectionId& collectionId) const;

    /// The unfinished operations somebody still has to settle.
    ///
    /// What the browser surfaces when a collection is opened: an operation that
    /// left nothing behind is not a recovery task, only an unfinished one.
    [[nodiscard]] QList<OperationRecord>
    needingRecovery(const domain::CollectionId& collectionId) const;

    /// Reconcile a journal after a crash or an interrupted run, putting back
    /// everything still in staging.
    bool recover(const domain::OperationId& operationId, QString* error);

    /// Ask Trash again for the groups still complete in staging.
    bool retryTrash(const domain::OperationId& operationId, QString* error);

    /// Record that a person has confirmed the groups with an undetermined
    /// outcome are in Trash. Moves nothing.
    bool confirmTrashed(const domain::OperationId& operationId, QString* error);

signals:
    void progressChanged(int completedGroups, int totalGroups);
    void recordChanged(const cullfinch::application::OperationRecord& record);
    void planInvalidated(const cullfinch::domain::PlanningResult& blockers);
    void errorOccurred(const QString& message);

private:
    bool persist(const OperationRecord& record, QString* error);
    /// Re-enumerate the plan against the filesystem. Emits `planInvalidated`
    /// and fails when what was reviewed no longer holds.
    bool verifyAgainstDisk(const domain::OperationPlan& plan, const domain::PhotoAssetList& current,
                           QString* error);
    /// Stage every group, journalling and reporting progress between them, and
    /// stop at the first group that fails or leaves recoverable work.
    bool stageGroups(const domain::OperationPlan& plan, OperationRecord& record,
                     const JournalWriter& journal, QString* error);
    /// Load, apply one executor recovery step, journal the outcome and report
    /// whether the operation is settled. The three offers differ only in the
    /// step they run, so they share everything around it -- including the rule
    /// that the journal is written before the caller is told anything.
    bool applyRecovery(const domain::OperationId& operationId,
                       OperationRecord (IOperationExecutor::*apply)(const OperationRecord&),
                       QString* error);

    IAssetRepository& repository_;
    IOperationExecutor& executor_;
};

} // namespace cullfinch::application

Q_DECLARE_METATYPE(cullfinch::application::OperationRecord)
Q_DECLARE_METATYPE(cullfinch::domain::PlanningResult)
