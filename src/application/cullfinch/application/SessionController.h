// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/DispositionController.h>
#include <cullfinch/application/FlowRegistry.h>
#include <cullfinch/application/Services.h>
#include <cullfinch/domain/FlowContract.h>

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUndoStack>

#include <memory>

namespace cullfinch::application {

/// Owns the active comparison engine, its state revision and its undo stack.
///
/// The controller is flow-agnostic: it never inspects a flow payload, and the
/// completion rule comes from the flow's own summary. Adding a comparison mode
/// therefore requires no change here.
class SessionController : public QObject {
    Q_OBJECT

public:
    SessionController(FlowRegistry& registry, IAssetRepository& repository,
                      DispositionController& dispositions, QObject* parent = nullptr);
    ~SessionController() override;

    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;
    SessionController(SessionController&&) = delete;
    SessionController& operator=(SessionController&&) = delete;

    /// Freeze the eligible identifiers and create a session-local draft.
    bool start(const QString& flowId, const domain::SelectionSnapshot& selection,
               const domain::FlowOptions& options, QString* error);

    /// Validate a stored session's input identities and restore its draft.
    bool resume(const StoredSession& stored, const domain::PhotoAssetList& currentAssets,
                QString* error);

    /// Dispatch a flow-local action against the current state revision.
    bool dispatch(const QString& actionName, const QJsonObject& payload, QString* error);

    /// Dispatch an action that a view produced while rendering
    /// `expectedStateRevision`. A gesture that was started against an older
    /// layout is rejected before it reaches the engine, so a late click or a
    /// repeated key cannot decide a second match.
    bool dispatchAt(const QString& actionName, const QJsonObject& payload,
                    quint64 expectedStateRevision, QString* error);

    /// Persist the draft and return to the browser without applying anything.
    bool pause(QString* error);

    /// Atomically merge draft rejections into the collection deletion marks.
    /// Only decisions actually made become marks; a session may finish early.
    bool finish(QString* error);

    /// Drop the draft without changing previously stored marks.
    bool discard(QString* error);

    [[nodiscard]] bool isActive() const { return flow_ != nullptr; }
    [[nodiscard]] domain::SessionId sessionId() const { return sessionId_; }
    [[nodiscard]] QString flowId() const;
    [[nodiscard]] domain::FlowDescriptor descriptor() const;
    [[nodiscard]] const domain::FlowState& state() const { return state_; }
    [[nodiscard]] const domain::SelectionSnapshot& snapshot() const { return snapshot_; }
    [[nodiscard]] domain::FlowSummary summary() const;
    [[nodiscard]] QUndoStack* undoStack() { return &undoStack_; }

    /// True while an autosave write is outstanding.
    [[nodiscard]] bool hasUnsavedChanges() const { return unsaved_; }

    /// Flush any pending autosave. Pause, Finish and clean shutdown wait on it.
    bool flushPendingSave(QString* error);

    /// Applied by the undo commands; public for the same reason as in
    /// DispositionController.
    void applyState(const domain::FlowState& state);

signals:
    void sessionStarted(const QString& flowId);
    void sessionEnded(const QString& flowId, bool applied);
    void stateChanged(const cullfinch::domain::FlowState& state,
                      const cullfinch::domain::FlowSummary& summary);
    void errorOccurred(const QString& message);
    void savingChanged(bool saving, bool unsaved);

private:
    bool validateTransition(const domain::FlowState& next, const domain::DecisionDelta& delta,
                            QString* error) const;
    void scheduleAutosave();
    bool writeSession(SessionLifecycle lifecycle, QString* error);
    void clearSession();
    [[nodiscard]] StoredSession toStoredSession(SessionLifecycle lifecycle) const;

    FlowRegistry& registry_;
    IAssetRepository& repository_;
    DispositionController& dispositions_;

    std::unique_ptr<domain::IComparisonFlow> flow_;
    domain::SessionId sessionId_;
    domain::SelectionSnapshot snapshot_;
    domain::FlowState state_;
    QUndoStack undoStack_;
    QTimer autosaveTimer_;
    bool unsaved_ = false;
    bool saving_ = false;
    /// Set when a draft write failed. Operations that depend on unpersisted
    /// state stay blocked until a retry succeeds.
    bool savePending_ = false;
};

} // namespace cullfinch::application
