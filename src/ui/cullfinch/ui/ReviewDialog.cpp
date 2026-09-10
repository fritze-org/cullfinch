// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/ReviewDialog.h>

#include <QDialogButtonBox>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace cullfinch::ui {

ReviewDialog::ReviewDialog(const domain::PlanningResult& planning, QWidget* parent)
    : QDialog(parent), planning_(planning) {
    setObjectName(QStringLiteral("reviewDialog"));
    setWindowTitle(tr("Review file operations"));
    resize(720, 520);

    auto* layout = new QVBoxLayout(this);

    auto* summary = new QLabel(this);
    summary->setObjectName(QStringLiteral("reviewSummary"));
    summary->setWordWrap(true);
    summary->setText(tr("%1 photos · %2 files · %3")
                         .arg(planning_.plan.logicalPhotoCount())
                         .arg(planning_.plan.physicalFileCount())
                         .arg(QLocale::system().formattedDataSize(planning_.plan.totalBytes())));
    layout->addWidget(summary);

    tree_ = new QTreeWidget(this);
    tree_->setObjectName(QStringLiteral("reviewTree"));
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({tr("Photo"), tr("Role"), tr("Path")});
    tree_->setRootIsDecorated(true);
    layout->addWidget(tree_, 1);

    auto* note = new QLabel(this);
    note->setObjectName(QStringLiteral("reviewNote"));
    note->setWordWrap(true);
    note->setText(
        tr("Each photo is moved to Trash as a complete group: the JPG and every associated RAW "
           "file together. The group first moves into a staging directory beside the photos, then "
           "that whole directory goes to Trash, so it appears in Trash as a folder. Restoring it "
           "from the desktop will not put the individual files back — use cullfinch to restore."));
    layout->addWidget(note);

    if (planning_.hasBlockers()) {
        auto* blockers = new QLabel(this);
        blockers->setObjectName(QStringLiteral("reviewBlockers"));
        blockers->setWordWrap(true);
        QStringList lines;
        for (const domain::PlanningIssue& issue : planning_.blocked) {
            lines.append(tr("%1 — %2").arg(issue.displayName, issue.reason));
        }
        blockers->setText(tr("%1 photo(s) cannot be moved:\n%2")
                              .arg(planning_.blocked.size())
                              .arg(lines.join(QLatin1Char('\n'))));
        layout->addWidget(blockers);
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("reviewButtons"));
    QPushButton* execute = buttons->addButton(tr("Move to Trash"), QDialogButtonBox::AcceptRole);
    execute->setObjectName(QStringLiteral("reviewExecute"));
    execute->setEnabled(!planning_.plan.isEmpty());
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        executionRequested_ = true;
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    populate();
}

void ReviewDialog::populate() {
    for (const domain::PlannedGroup& group : planning_.plan.groups) {
        auto* item = new QTreeWidgetItem(tree_);
        item->setText(0, group.displayName);
        item->setText(1, tr("%1 files").arg(group.members.size()));
        item->setText(2, QLocale::system().formattedDataSize(group.totalBytes()));

        // The complete group membership is visible: no file is moved without
        // being shown here first.
        for (const domain::PlannedMember& member : group.members) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, member.fileName);
            child->setText(1, domain::memberRoleName(member.role));
            child->setText(2, member.sourcePath);
        }
        item->setExpanded(true);
    }
    tree_->resizeColumnToContents(0);
}

} // namespace cullfinch::ui
