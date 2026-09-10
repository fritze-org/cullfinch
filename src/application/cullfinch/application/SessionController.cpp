// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/SessionController.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QUndoCommand>

namespace cullfinch::application {
namespace {

using domain::AssetId;
using domain::FlowState;

/// One complete decision, stored as the flow states either side of it.
///
/// Flow states are small JSON payloads, so this keeps undo exact without
/// copying the collection for every rejection. Redo invalidation and action
/// state come from QUndoStack.
class FlowTransitionCommand : public QUndoCommand {
public:
    FlowTransitionCommand(SessionController* controller, FlowState before, FlowState after,
                          const QString& text)
        : QUndoCommand(text), controller_(controller), before_(std::move(before)),
          after_(std::move(after)) {}

    void redo() override { controller_->applyState(after_); }
    void undo() override { controller_->applyState(before_); }

private:
    SessionController* controller_;
    FlowState before_;
    FlowState after_;
};

} // namespace

SessionController::SessionController(FlowRegistry& registry, IAssetRepository& repository,
                                     DispositionController& dispositions, QObject* parent)
    : QObject(parent), registry_(registry), repository_(repository), dispositions_(dispositions) {
    autosaveTimer_.setSingleShot(true);
    // Coalesces superseded unsaved snapshots: rapid culling produces one write
    // per quiet moment rather than one per click.
    autosaveTimer_.setInterval(250);
    connect(&autosaveTimer_, &QTimer::timeout, this, [this]() {
        QString error;
        if (!writeSession(SessionLifecycle::Active, &error)) {
            Q_EMIT errorOccurred(error);
        }
    });
}

SessionController::~SessionController() = default;

QString SessionController::flowId() const {
    return flow_ != nullptr ? flow_->descriptor().id : QString();
}

domain::FlowDescriptor SessionController::descriptor() const {
    return flow_ != nullptr ? flow_->descriptor() : domain::FlowDescriptor{};
}

domain::FlowSummary SessionController::summary() const {
    return flow_ != nullptr ? flow_->summarise(state_) : domain::FlowSummary{};
}

bool SessionController::start(const QString& flowId, const domain::SelectionSnapshot& selection,
                              const domain::FlowOptions& options, QString* error) {
    if (isActive()) {
        if (error != nullptr) {
            *error = tr("A comparison is already running. Finish, pause or discard it first.");
        }
        return false;
    }

    std::unique_ptr<domain::IComparisonFlow> flow = registry_.create(flowId);
    if (flow == nullptr) {
        if (error != nullptr) {
            *error = tr("Unknown comparison flow '%1'.").arg(flowId);
        }
        return false;
    }

    const domain::FlowDescriptor descriptor = flow->descriptor();
    const auto inputSize = static_cast<int>(selection.size());
    if (inputSize < descriptor.minimumInputSize) {
        if (error != nullptr) {
            *error = tr("%1 needs at least %2 eligible photos; %3 were selected.")
                         .arg(descriptor.displayName)
                         .arg(descriptor.minimumInputSize)
                         .arg(inputSize);
        }
        return false;
    }
    if (descriptor.maximumInputSize > 0 && inputSize > descriptor.maximumInputSize) {
        if (error != nullptr) {
            *error = tr("%1 accepts at most %2 photos; %3 were selected.")
                         .arg(descriptor.displayName)
                         .arg(descriptor.maximumInputSize)
                         .arg(inputSize);
        }
        return false;
    }

    const domain::ValidationResult validation = flow->validate(selection, options);
    if (!validation.valid) {
        if (error != nullptr) {
            *error = validation.message;
        }
        return false;
    }

    flow_ = std::move(flow);
    sessionId_ = domain::SessionId::generate();
    snapshot_ = selection;
    state_ = flow_->initialise(selection, options);
    undoStack_.clear();
    unsaved_ = true;
    savePending_ = false;

    // Only one draft is active at a time, and collection marks cannot change
    // underneath it.
    dispositions_.setMarkingEnabled(false);

    QString writeError;
    if (!writeSession(SessionLifecycle::Active, &writeError)) {
        // The draft exists in memory; the user is told it is not yet durable.
        Q_EMIT errorOccurred(writeError);
    }

    Q_EMIT sessionStarted(descriptor.id);
    Q_EMIT stateChanged(state_, summary());
    return true;
}

bool SessionController::resume(const StoredSession& stored,
                               const domain::PhotoAssetList& currentAssets, QString* error) {
    if (isActive()) {
        if (error != nullptr) {
            *error = tr("A comparison is already running.");
        }
        return false;
    }

    std::unique_ptr<domain::IComparisonFlow> flow = registry_.create(stored.draft.flowId);
    if (flow == nullptr) {
        if (error != nullptr) {
            // The record is preserved and reported as incompatible; it is never
            // interpreted as another mode.
            *error = tr("The saved comparison uses flow '%1', which this build does not provide. "
                        "Its record is kept and your deletion marks remain available.")
                         .arg(stored.draft.flowId);
        }
        return false;
    }

    // Validate the input identities before restoring anything.
    QHash<AssetId, quint64> currentRevisions;
    for (const domain::PhotoAsset& asset : currentAssets) {
        currentRevisions.insert(asset.id, asset.membershipRevision);
    }
    QStringList changed;
    for (const AssetId& id : stored.snapshot.orderedAssetIds) {
        const auto current = currentRevisions.constFind(id);
        if (current == currentRevisions.constEnd()) {
            changed.append(tr("a photo is no longer present"));
            break;
        }
        if (*current != stored.snapshot.membershipRevisions.value(id)) {
            changed.append(tr("a photo's file group changed"));
            break;
        }
    }
    if (!changed.isEmpty()) {
        if (error != nullptr) {
            *error = tr("The saved comparison cannot be resumed because %1. Start a new "
                        "comparison; your deletion marks are unaffected.")
                         .arg(changed.first());
        }
        return false;
    }

    const domain::RestoreResult restored = flow->restore(stored.draft);
    if (!restored.restored) {
        if (error != nullptr) {
            *error = restored.message;
        }
        return false;
    }

    flow_ = std::move(flow);
    sessionId_ = stored.id;
    snapshot_ = stored.snapshot;
    state_ = restored.state;
    // Resume starts an empty undo stack at the restored draft state: history is
    // retained only within a process in this release.
    undoStack_.clear();
    unsaved_ = false;
    savePending_ = false;
    dispositions_.setMarkingEnabled(false);

    Q_EMIT sessionStarted(flow_->descriptor().id);
    Q_EMIT stateChanged(state_, summary());
    return true;
}

bool SessionController::dispatch(const QString& actionName, const QJsonObject& payload,
                                 QString* error) {
    return dispatchAt(actionName, payload, state_.revision, error);
}

bool SessionController::dispatchAt(const QString& actionName, const QJsonObject& payload,
                                   quint64 expectedStateRevision, QString* error) {
    if (!isActive()) {
        if (error != nullptr) {
            *error = tr("No comparison is running.");
        }
        return false;
    }

    // Stale or duplicate actions are rejected before dispatch, so one gesture
    // can never decide two matches.
    if (expectedStateRevision != state_.revision) {
        if (error != nullptr) {
            *error = tr("This decision refers to an earlier state and was ignored.");
        }
        return false;
    }

    domain::FlowAction action;
    action.sessionId = sessionId_;
    action.flowId = flow_->descriptor().id;
    action.name = actionName;
    action.payload = payload;
    action.expectedStateRevision = expectedStateRevision;

    const domain::TransitionResult result = flow_->reduce(state_, action);
    if (!result.accepted) {
        if (error != nullptr) {
            *error = result.message;
        }
        return false;
    }

    if (!validateTransition(result.state, result.delta, error)) {
        return false;
    }

    const domain::FlowState before = state_;
    undoStack_.push(new FlowTransitionCommand(
        this, before, result.state, tr("%1 decision").arg(flow_->descriptor().displayName)));
    return true;
}

void SessionController::applyState(const domain::FlowState& state) {
    state_ = state;
    unsaved_ = true;
    scheduleAutosave();
    Q_EMIT stateChanged(state_, summary());
}

bool SessionController::validateTransition(const domain::FlowState& next,
                                           const domain::DecisionDelta& delta,
                                           QString* error) const {
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (next.flowId != state_.flowId || next.schemaVersion != state_.schemaVersion) {
        return fail(tr("The comparison engine returned state for a different flow."));
    }
    if (next.revision <= state_.revision) {
        return fail(tr("The comparison engine did not advance its state revision."));
    }

    const QSet<AssetId> input(snapshot_.orderedAssetIds.cbegin(), snapshot_.orderedAssetIds.cend());

    // Every affected identifier must belong to the frozen input: a flow can
    // neither reject an unselected photo nor invent one.
    for (const AssetId& id : delta.rejected) {
        if (!input.contains(id)) {
            return fail(tr("The comparison engine tried to reject a photo that was not selected."));
        }
    }
    for (const AssetId& id : delta.restored) {
        if (!input.contains(id)) {
            return fail(
                tr("The comparison engine tried to restore a photo that was not selected."));
        }
    }

    const domain::FlowSummary summary = flow_->summarise(next);
    const QSet<AssetId> remaining(summary.remaining.cbegin(), summary.remaining.cend());
    const QSet<AssetId> rejected(summary.draftRejected.cbegin(), summary.draftRejected.cend());

    if (remaining.size() != summary.remaining.size() ||
        rejected.size() != summary.draftRejected.size()) {
        return fail(tr("The comparison engine reported a photo more than once."));
    }
    if (remaining.intersects(rejected)) {
        return fail(tr("The comparison engine reported a photo as both remaining and eliminated."));
    }
    if ((remaining | rejected) != input) {
        return fail(tr("The comparison engine changed the set of photos under comparison."));
    }
    return true;
}

void SessionController::scheduleAutosave() {
    if (!autosaveTimer_.isActive()) {
        autosaveTimer_.start();
    }
    Q_EMIT savingChanged(saving_, unsaved_);
}

StoredSession SessionController::toStoredSession(SessionLifecycle lifecycle) const {
    StoredSession stored;
    stored.id = sessionId_;
    stored.collectionId = snapshot_.collectionId;
    stored.snapshot = snapshot_;
    stored.draft.flowId = state_.flowId;
    stored.draft.schemaVersion = state_.schemaVersion;
    stored.draft.revision = state_.revision;
    stored.draft.payload = state_.payload;
    stored.draftRejected =
        flow_ != nullptr ? flow_->summarise(state_).draftRejected : QList<AssetId>{};
    stored.lifecycle = lifecycle;
    stored.updatedUtc = QDateTime::currentDateTimeUtc();
    return stored;
}

bool SessionController::writeSession(SessionLifecycle lifecycle, QString* error) {
    if (!isActive()) {
        return true;
    }
    saving_ = true;
    Q_EMIT savingChanged(saving_, unsaved_);

    QString storageError;
    const bool written = repository_.saveSession(toStoredSession(lifecycle), &storageError);

    saving_ = false;
    if (!written) {
        // Keep the in-memory draft and allow a retry. Anything that depends on
        // persisted state stays blocked.
        savePending_ = true;
        Q_EMIT savingChanged(saving_, unsaved_);
        if (error != nullptr) {
            *error = tr("The comparison draft could not be saved: %1").arg(storageError);
        }
        return false;
    }

    savePending_ = false;
    unsaved_ = false;
    Q_EMIT savingChanged(saving_, unsaved_);
    return true;
}

bool SessionController::flushPendingSave(QString* error) {
    if (!isActive()) {
        return true;
    }
    autosaveTimer_.stop();
    if (!unsaved_ && !savePending_) {
        return true;
    }
    return writeSession(SessionLifecycle::Active, error);
}

bool SessionController::pause(QString* error) {
    if (!isActive()) {
        return true;
    }
    autosaveTimer_.stop();
    if (!writeSession(SessionLifecycle::Paused, error)) {
        return false;
    }
    const QString id = flow_->descriptor().id;
    clearSession();
    Q_EMIT sessionEnded(id, false);
    return true;
}

bool SessionController::finish(QString* error) {
    if (!isActive()) {
        return true;
    }

    const domain::FlowSummary summary = flow_->summarise(state_);
    if (!summary.canFinish) {
        if (error != nullptr) {
            *error = tr("This comparison cannot be finished yet.");
        }
        return false;
    }

    autosaveTimer_.stop();
    if (!writeSession(SessionLifecycle::Active, error)) {
        return false;
    }

    const QString id = flow_->descriptor().id;
    const QString commandText =
        tr("Apply %1 rejections").arg(flow_->descriptor().displayName.toLower());

    // Marking must be possible again before the marks are applied.
    dispositions_.setMarkingEnabled(true);
    if (!dispositions_.applyRejections(summary.draftRejected, commandText, error)) {
        dispositions_.setMarkingEnabled(false);
        return false;
    }

    QString storageError;
    if (!repository_.saveSession(toStoredSession(SessionLifecycle::Finished), &storageError)) {
        // The marks are durable; only the session record lagged behind.
        Q_EMIT errorOccurred(
            tr("Deletion marks were applied, but the comparison record could not be updated: %1")
                .arg(storageError));
    }

    clearSession();
    Q_EMIT sessionEnded(id, true);
    return true;
}

bool SessionController::discard(QString* error) {
    if (!isActive()) {
        return true;
    }
    autosaveTimer_.stop();

    const QString id = flow_->descriptor().id;
    QString storageError;
    if (!repository_.deleteSession(sessionId_, &storageError)) {
        if (error != nullptr) {
            *error = tr("The comparison draft could not be discarded: %1").arg(storageError);
        }
        return false;
    }

    clearSession();
    Q_EMIT sessionEnded(id, false);
    return true;
}

void SessionController::clearSession() {
    flow_.reset();
    state_ = domain::FlowState{};
    snapshot_ = domain::SelectionSnapshot{};
    sessionId_ = domain::SessionId{};
    undoStack_.clear();
    unsaved_ = false;
    savePending_ = false;
    dispositions_.setMarkingEnabled(true);
    Q_EMIT savingChanged(false, false);
}

} // namespace cullfinch::application
