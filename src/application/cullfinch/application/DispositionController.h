// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>
#include <cullfinch/domain/Ids.h>
#include <cullfinch/domain/PhotoAsset.h>

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUndoStack>

namespace cullfinch::application {

/// Owns collection-level deletion marks and their undo history.
///
/// Every change is persisted before the corresponding in-memory undo-stack
/// transition is published. A QUndoCommand therefore never performs unreported
/// asynchronous I/O: the command calls back into this controller, which has a
/// failure channel. On a storage failure the authoritative marks are left
/// unchanged, the history is dropped rather than silently desynchronised, and
/// the collection must be reloaded.
class DispositionController : public QObject {
    Q_OBJECT

public:
    explicit DispositionController(IAssetRepository& repository, QObject* parent = nullptr);

    void setCollection(const domain::CollectionId& id, quint64 revision);

    /// Follow the collection revision without discarding the undo history.
    /// A rescan of our own making advances the revision legitimately; only
    /// a change we did not make is a real conflict.
    void setRevision(quint64 revision) { revision_ = revision; }
    [[nodiscard]] domain::CollectionId collectionId() const { return collectionId_; }
    [[nodiscard]] quint64 revision() const { return revision_; }

    /// Merge a finished comparison's draft rejections into the collection
    /// marks as one undoable step.
    bool applyRejections(const QList<domain::AssetId>& ids, const QString& commandText,
                         QString* error);

    /// Clear the deletion mark on assets, making them eligible again.
    bool unmark(const QList<domain::AssetId>& ids, QString* error);

    [[nodiscard]] QUndoStack* undoStack() { return &undoStack_; }

    /// True once a storage failure has invalidated the undo history.
    [[nodiscard]] bool isBlocked() const { return blocked_; }

    /// Set while another instance holds the collection's writer lock. Every
    /// mark change is refused; nothing about the history is invalidated.
    void setReadOnly(bool readOnly);
    [[nodiscard]] bool isReadOnly() const { return readOnly_; }

    /// Enabled while no comparison draft is active: changing collection marks
    /// underneath a running session is not allowed.
    void setMarkingEnabled(bool enabled);
    [[nodiscard]] bool isMarkingEnabled() const { return markingEnabled_; }

    /// Persist a mark change. Public so the command type stays a plain
    /// implementation detail of this translation unit.
    bool persist(const QHash<domain::AssetId, domain::Disposition>& targets, QString* error);

    /// Called by the undo commands from inside QUndoStack::undo()/redo().
    /// A refused write must not clear the stack synchronously from here:
    /// that would delete the executing command.
    void applyFromHistory(const QHash<domain::AssetId, domain::Disposition>& targets);

signals:
    void dispositionsChanged(const QList<cullfinch::domain::AssetId>& affected, quint64 revision);
    void errorOccurred(const QString& message);
    void blockedChanged(bool blocked);
    void markingEnabledChanged(bool enabled);

private:
    IAssetRepository& repository_;
    QUndoStack undoStack_;
    domain::CollectionId collectionId_;
    quint64 revision_ = 0;
    bool blocked_ = false;
    bool readOnly_ = false;
    bool markingEnabled_ = true;
    /// True while a QUndoCommand is executing against this controller.
    bool applyingFromHistory_ = false;
};

} // namespace cullfinch::application
