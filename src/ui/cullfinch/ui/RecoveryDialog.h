// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/OperationController.h>

#include <QDialog>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QTreeWidget>

namespace cullfinch::ui {

/// What a file operation that never finished left behind, and the offers that
/// can settle it.
///
/// The staging journal already records everything recovery needs; without a
/// screen for it a person who crashed mid-operation just finds photos missing
/// from the collection, with nothing to say they are sitting in the staging
/// directory or already in Trash. Each offer is exactly one of the things the
/// executor can actually do, no more: nothing here deletes, and nothing here
/// guesses where a file went.
class RecoveryDialog : public QDialog {
    Q_OBJECT

public:
    /// @param writable false in a read-only instance, which may look at the
    ///        records but takes none of the offers -- repairing a collection
    ///        belongs to the window that holds the writer lock.
    RecoveryDialog(application::OperationController& operations,
                   const domain::CollectionId& collectionId, bool writable,
                   QWidget* parent = nullptr);

    /// True once an offer has been taken up, so the caller knows the files on
    /// disk may have moved.
    [[nodiscard]] bool anythingChanged() const { return changed_; }

    [[nodiscard]] int recordCount() const { return static_cast<int>(records_.size()); }

private:
    /// The three things this screen can ask of a stopped operation.
    enum class Offer { Restore, RetryTrash, ConfirmTrashed };

    void buildLayout();
    /// Re-read the journal and rebuild the list. Every offer persists before it
    /// answers, so this is how the screen follows what just happened.
    void reload();
    void populate();
    /// One operation's row, one group's row under it, one row per file under
    /// that. Split apart because the three levels of nesting are the shape of
    /// the data, not of the decision being made.
    void addRecordItem(const application::OperationRecord& record, int row);
    void addGroupItem(QTreeWidgetItem* parent, const application::OperationRecord& record,
                      const domain::PlannedGroup& group, int row);
    void updateOffers();
    /// Take one offer on the selected record and report its outcome.
    void take(Offer offer, const QString& settled);
    [[nodiscard]] const application::OperationRecord* selectedRecord() const;

    application::OperationController& operations_;
    domain::CollectionId collectionId_;
    bool writable_ = true;
    bool changed_ = false;

    QList<application::OperationRecord> records_;
    QLabel* summary_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* detail_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* retryTrash_ = nullptr;
    QPushButton* confirmTrashed_ = nullptr;
};

} // namespace cullfinch::ui
