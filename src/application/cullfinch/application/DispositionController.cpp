// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/DispositionController.h>

#include <QCoreApplication>
#include <QUndoCommand>

#include <utility>

namespace cullfinch::application {
namespace {

using domain::AssetId;
using domain::Disposition;

/// Forward and inverse mark changes, kept as a per-asset map rather than a copy
/// of the collection.
class MarkCommand : public QUndoCommand {
public:
    MarkCommand(DispositionController* controller, QHash<AssetId, Disposition> forward,
                QHash<AssetId, Disposition> inverse, const QString& text)
        : QUndoCommand(text), controller_(controller), forward_(std::move(forward)),
          inverse_(std::move(inverse)) {}

    void redo() override {
        if (!firstRedoConsumed_) {
            // The change was already persisted before this command was pushed.
            firstRedoConsumed_ = true;
            return;
        }
        apply(forward_);
    }

    void undo() override { apply(inverse_); }

private:
    void apply(const QHash<AssetId, Disposition>& targets) {
        // QUndoStack has already moved its index by the time this runs, so a
        // refused write leaves the stack describing a transition storage never
        // saw. The controller resolves that by dropping the history -- but it
        // must not do so *here*: clearing the stack deletes this command while
        // it is still executing. The controller therefore defers the clear
        // until control has left the command.
        controller_->applyFromHistory(targets);
    }

    DispositionController* controller_;
    QHash<AssetId, Disposition> forward_;
    QHash<AssetId, Disposition> inverse_;
    bool firstRedoConsumed_ = false;
};

} // namespace

DispositionController::DispositionController(IAssetRepository& repository, QObject* parent)
    : QObject(parent), repository_(repository) {}

void DispositionController::setCollection(const domain::CollectionId& id, quint64 revision) {
    collectionId_ = id;
    revision_ = revision;
    undoStack_.clear();
    if (blocked_) {
        blocked_ = false;
        Q_EMIT blockedChanged(false);
    }
}

void DispositionController::setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
}

void DispositionController::setMarkingEnabled(bool enabled) {
    if (markingEnabled_ == enabled) {
        return;
    }
    markingEnabled_ = enabled;
    Q_EMIT markingEnabledChanged(enabled);
}

bool DispositionController::writeDispositions(const QHash<AssetId, Disposition>& targets,
                                              quint64* newRevision, QString* error) {
    if (!collectionId_.isValid()) {
        if (error != nullptr) {
            *error = tr("No collection is open.");
        }
        return false;
    }

    QList<AssetId> reject;
    QList<AssetId> neutral;
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) {
        if (it.value() == Disposition::Reject) {
            reject.append(it.key());
        } else {
            neutral.append(it.key());
        }
    }

    if (QString storageError; !repository_.applyDispositions(collectionId_, revision_, reject,
                                                             neutral, newRevision, &storageError)) {
        if (error != nullptr) {
            *error = std::move(storageError);
        }
        if (!blocked_) {
            blocked_ = true;
            if (applyingFromHistory_) {
                // The stack is mid-call into the command that brought us
                // here; deleting it now is a use-after-free. Drop the history
                // once the stack has returned to the event loop. `blocked_`
                // already refuses new pushes in the meantime.
                QMetaObject::invokeMethod(
                    this, [this]() { undoStack_.clear(); }, Qt::QueuedConnection);
            } else {
                undoStack_.clear();
            }
            Q_EMIT blockedChanged(true);
        }
        return false;
    }
    return true;
}

bool DispositionController::persist(const QHash<AssetId, Disposition>& targets, QString* error) {
    if (targets.isEmpty()) {
        return true;
    }

    quint64 newRevision = revision_;
    if (!writeDispositions(targets, &newRevision, error)) {
        return false;
    }

    revision_ = newRevision;
    Q_EMIT dispositionsChanged(targets.keys(), revision_);
    return true;
}

void DispositionController::applyFromHistory(const QHash<AssetId, Disposition>& targets) {
    QString error;
    applyingFromHistory_ = true;
    const bool written = persist(targets, &error);
    applyingFromHistory_ = false;
    if (!written) {
        // The authoritative marks are unchanged; the history can no longer be
        // trusted to describe storage, so it is being dropped.
        Q_EMIT errorOccurred(error);
    }
}

std::optional<DispositionController::PendingRejections>
DispositionController::beginRejections(const QList<AssetId>& ids, QString* error) {
    if (ids.isEmpty()) {
        return PendingRejections{};
    }
    if (readOnly_) {
        if (error != nullptr) {
            *error = tr("This collection is open read-only because another Cullfinch window has "
                        "it open; deletion marks cannot change here.");
        }
        return std::nullopt;
    }
    if (blocked_) {
        if (error != nullptr) {
            *error = tr("Deletion marks are unavailable until the collection is reloaded.");
        }
        return std::nullopt;
    }

    PendingRejections pending;
    for (const AssetId& id : ids) {
        pending.forward.insert(id, Disposition::Reject);
        // Finishing a session never erases marks made by another session, so
        // the inverse of "mark rejected" is "make neutral" only for assets this
        // step actually changed. Assets already rejected are not included.
        pending.inverse.insert(id, Disposition::Neutral);
    }

    if (!writeDispositions(pending.forward, nullptr, error)) {
        return std::nullopt;
    }
    return pending;
}

void DispositionController::commitRejections(const PendingRejections& pending,
                                             const QString& commandText) {
    if (pending.forward.isEmpty()) {
        return;
    }
    // The write already happened; only the revision this controller reports
    // and the undo step were waiting on the caller's transaction to commit.
    revision_ = repository_.collectionRevision(collectionId_, nullptr);
    Q_EMIT dispositionsChanged(pending.forward.keys(), revision_);
    undoStack_.push(new MarkCommand(this, pending.forward, pending.inverse, commandText));
}

bool DispositionController::applyRejections(const QList<AssetId>& ids, const QString& commandText,
                                            QString* error) {
    const std::optional<PendingRejections> pending = beginRejections(ids, error);
    if (!pending.has_value()) {
        return false;
    }
    commitRejections(*pending, commandText);
    return true;
}

bool DispositionController::unmark(const QList<AssetId>& ids, QString* error) {
    if (ids.isEmpty()) {
        return true;
    }
    if (!markingEnabled_) {
        if (error != nullptr) {
            *error = tr("Finish or discard the active comparison before changing deletion marks.");
        }
        return false;
    }
    if (readOnly_) {
        if (error != nullptr) {
            *error = tr("This collection is open read-only because another Cullfinch window has "
                        "it open; deletion marks cannot change here.");
        }
        return false;
    }
    if (blocked_) {
        if (error != nullptr) {
            *error = tr("Deletion marks are unavailable until the collection is reloaded.");
        }
        return false;
    }

    QHash<AssetId, Disposition> forward;
    QHash<AssetId, Disposition> inverse;
    for (const AssetId& id : ids) {
        forward.insert(id, Disposition::Neutral);
        inverse.insert(id, Disposition::Reject);
    }

    if (!persist(forward, error)) {
        return false;
    }

    undoStack_.push(new MarkCommand(this, forward, inverse,
                                    tr("Unmark %1 photos").arg(static_cast<int>(ids.size()))));
    return true;
}

} // namespace cullfinch::application
