// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/RecoveryDialog.h>

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QLocale>
#include <QSignalBlocker>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace cullfinch::ui {
namespace {

/// The row index of the record a tree item belongs to.
constexpr int kRecordRole = Qt::UserRole + 1;

/// Where a member's files are, in the words a person can act on.
///
/// Deliberately not the journal token: "staged" says nothing about what to do,
/// and the whole point of this screen is that the durable step decides what can
/// be offered.
QString stepDescription(const QString& step) {
    namespace steps = application::operationStep;
    if (step == steps::planned) {
        return QCoreApplication::translate("cullfinch", "At its original location");
    }
    if (step == steps::staged) {
        return QCoreApplication::translate("cullfinch", "In the staging directory");
    }
    if (step == steps::restored) {
        return QCoreApplication::translate("cullfinch", "Put back at its original location");
    }
    if (step == steps::trashed) {
        return QCoreApplication::translate("cullfinch", "In Trash");
    }
    if (step == steps::uncertain) {
        return QCoreApplication::translate("cullfinch", "Unknown — probably in Trash");
    }
    return step;
}

} // namespace

RecoveryDialog::RecoveryDialog(application::OperationController& operations,
                               const domain::CollectionId& collectionId, bool writable,
                               QWidget* parent)
    : QDialog(parent), operations_(operations), collectionId_(collectionId), writable_(writable) {
    setObjectName(QStringLiteral("recoveryDialog"));
    setWindowTitle(tr("Unfinished file operations"));
    resize(760, 540);

    buildLayout();
    reload();
}

void RecoveryDialog::buildLayout() {
    auto* layout = new QVBoxLayout(this);

    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("recoverySummary"));
    summary_->setWordWrap(true);
    layout->addWidget(summary_);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("recoveryTree"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Photo"), tr("Where"), tr("Detail")});
    tree_->setRootIsDecorated(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(tree_, 1);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem*, QTreeWidgetItem*) { updateOffers(); });

    detail_ = new QLabel(this);
    detail_->setObjectName(QStringLiteral("recoveryDetail"));
    detail_->setWordWrap(true);
    detail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(detail_);

    auto* note = new QLabel(this);
    note->setObjectName(QStringLiteral("recoveryNote"));
    note->setWordWrap(true);
    note->setText(tr(
        "A photo is moved to Trash as a complete group, by way of a staging directory beside the "
        "photos. An operation that was interrupted can leave a group sitting in staging, or can "
        "leave nothing able to say whether the group reached Trash. Restore puts staged files "
        "back where they came from. Retry Trash asks Trash again for a group that is still "
        "complete in staging. Confirmed in Trash only records what you have seen yourself — "
        "check the Trash first. Nothing here ever deletes anything again."));
    layout->addWidget(note);

    if (!writable_) {
        auto* readOnly = new QLabel(this);
        readOnly->setObjectName(QStringLiteral("recoveryReadOnly"));
        readOnly->setWordWrap(true);
        readOnly->setText(tr("This collection is open read-only, so these operations can be read "
                             "but not repaired here. Repairing one belongs to the window that has "
                             "the collection open for writing."));
        layout->addWidget(readOnly);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setObjectName(QStringLiteral("recoveryButtons"));
    restore_ = buttons->addButton(tr("Restore"), QDialogButtonBox::ActionRole);
    restore_->setObjectName(QStringLiteral("recoveryRestore"));
    retryTrash_ = buttons->addButton(tr("Retry Trash"), QDialogButtonBox::ActionRole);
    retryTrash_->setObjectName(QStringLiteral("recoveryRetryTrash"));
    confirmTrashed_ = buttons->addButton(tr("Confirmed in Trash"), QDialogButtonBox::ActionRole);
    confirmTrashed_->setObjectName(QStringLiteral("recoveryConfirmTrashed"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(restore_, &QPushButton::clicked, this, [this]() {
        take(Offer::Restore,
             tr("The staged files were put back where they came from. The photos are still marked "
                "for deletion; review file operations again to move them."));
    });
    connect(retryTrash_, &QPushButton::clicked, this, [this]() {
        take(Offer::RetryTrash, tr("The staged group reached Trash. The operation is complete."));
    });
    connect(confirmTrashed_, &QPushButton::clicked, this, [this]() {
        take(Offer::ConfirmTrashed,
             tr("Recorded as confirmed in Trash. Nothing was moved or deleted."));
    });
}

void RecoveryDialog::reload() {
    records_ = operations_.needingRecovery(collectionId_);
    populate();
    summary_->setText(records_.isEmpty()
                          ? tr("No file operation for this collection needs attention.")
                          : tr("%1 file operation(s) did not finish. Select one to see what it "
                               "left behind.")
                                .arg(records_.size()));
    updateOffers();
}

void RecoveryDialog::populate() {
    // Rebuilding the tree moves the current item through every intermediate
    // state, including the moment clear() is destroying the item the selection
    // points at. Nothing should read a selection that is mid-rebuild; the
    // caller asks for the offers once the tree is whole again.
    const QSignalBlocker blocker(tree_);
    tree_->clear();
    for (int row = 0; row < records_.size(); ++row) {
        addRecordItem(records_.at(row), row);
    }
    tree_->resizeColumnToContents(0);
    if (tree_->topLevelItemCount() > 0) {
        tree_->setCurrentItem(tree_->topLevelItem(0));
    }
}

void RecoveryDialog::addRecordItem(const application::OperationRecord& record, int row) {
    auto* item = new QTreeWidgetItem(tree_);
    // Every row carries its record, children included, so clicking a file
    // selects the operation it belongs to rather than nothing.
    item->setData(0, kRecordRole, row);
    item->setText(0, tr("%1 photo(s), reviewed %2")
                         .arg(record.plan.logicalPhotoCount())
                         .arg(QLocale::system().toString(record.createdUtc.toLocalTime(),
                                                         QLocale::ShortFormat)));
    item->setText(1, domain::operationStateName(record.state));
    item->setText(2, record.error);
    item->setToolTip(2, record.error);

    for (const domain::PlannedGroup& group : record.plan.groups) {
        addGroupItem(item, record, group, row);
    }
    item->setExpanded(true);
}

void RecoveryDialog::addGroupItem(QTreeWidgetItem* parent,
                                  const application::OperationRecord& record,
                                  const domain::PlannedGroup& group, int row) {
    auto* groupItem = new QTreeWidgetItem(parent);
    groupItem->setData(0, kRecordRole, row);
    groupItem->setText(0, group.displayName);
    groupItem->setText(2,
                       QDir(record.plan.stagingRoot).absoluteFilePath(group.stagingDirectoryName));

    // The complete group membership is shown, as the review screen shows it: a
    // person deciding what to do about a half-moved photo needs to see every
    // file it owns, not a count.
    for (const domain::PlannedMember& member : group.members) {
        const auto entry = std::ranges::find(record.members, member.memberId,
                                             &application::OperationMemberRecord::memberId);
        auto* memberItem = new QTreeWidgetItem(groupItem);
        memberItem->setData(0, kRecordRole, row);
        memberItem->setText(0, member.fileName);
        if (entry == record.members.cend()) {
            continue;
        }
        memberItem->setText(1, stepDescription(entry->lastDurableStep));
        memberItem->setText(2, entry->error.isEmpty() ? member.sourcePath : entry->error);
    }
    groupItem->setExpanded(true);
}

const application::OperationRecord* RecoveryDialog::selectedRecord() const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr) {
        return nullptr;
    }
    bool known = false;
    const int row = item->data(0, kRecordRole).toInt(&known);
    if (!known || row < 0 || row >= records_.size()) {
        return nullptr;
    }
    return &records_.at(row);
}

void RecoveryDialog::updateOffers() {
    const application::OperationRecord* record = selectedRecord();
    const application::RecoveryOffers offers =
        record == nullptr ? application::RecoveryOffers{} : application::recoveryOffersFor(*record);

    // Read-only is refused here as it is on every other write path: the offers
    // are visible so it is clear what the writing instance can do, and inert.
    restore_->setEnabled(writable_ && offers.restore);
    retryTrash_->setEnabled(writable_ && offers.retryTrash);
    confirmTrashed_->setEnabled(writable_ && offers.confirmTrashed);

    // The selected record's own error text, so what a person is being asked to
    // decide about is never more than one click away.
    detail_->setText(record == nullptr ? QString() : record->error);
}

void RecoveryDialog::take(Offer offer, const QString& settled) {
    const application::OperationRecord* record = selectedRecord();
    if (record == nullptr) {
        return;
    }

    // Copied before the call: taking an offer rewrites the list this points
    // into.
    const domain::OperationId id = record->plan.id;
    QString error;
    bool done = false;
    switch (offer) {
    case Offer::Restore:
        done = operations_.recover(id, &error);
        break;
    case Offer::RetryTrash:
        done = operations_.retryTrash(id, &error);
        break;
    case Offer::ConfirmTrashed:
        done = operations_.confirmTrashed(id, &error);
        break;
    }

    // The journal is written whether or not the operation is settled, and a
    // partial retry really did move files, so the caller is told either way.
    changed_ = true;
    reload();
    // Set last: reload() has just put the reloaded record's own error here.
    detail_->setText(done ? settled : error);
}

} // namespace cullfinch::ui
