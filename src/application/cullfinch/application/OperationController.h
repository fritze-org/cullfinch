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

    /// Reconcile a journal after a crash or an interrupted run.
    bool recover(const domain::OperationId& operationId, QString* error);

signals:
    void progressChanged(int completedGroups, int totalGroups);
    void recordChanged(const cullfinch::application::OperationRecord& record);
    void planInvalidated(const cullfinch::domain::PlanningResult& blockers);
    void errorOccurred(const QString& message);

private:
    bool persist(const OperationRecord& record, QString* error);

    IAssetRepository& repository_;
    IOperationExecutor& executor_;
};

} // namespace cullfinch::application

Q_DECLARE_METATYPE(cullfinch::application::OperationRecord)
Q_DECLARE_METATYPE(cullfinch::domain::PlanningResult)
