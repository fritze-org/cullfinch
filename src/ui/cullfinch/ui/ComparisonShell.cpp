// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/ComparisonShell.h>

#include <QAction>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QVBoxLayout>

namespace cullfinch::ui {

ComparisonShell::ComparisonShell(application::SessionController& session,
                                 std::unique_ptr<IFlowView> view,
                                 const AssetPresentationMap& presentations, QWidget* parent)
    : QWidget(parent, Qt::Window), session_(session), view_(std::move(view)) {
    setObjectName(QStringLiteral("comparisonShell"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowTitle(session_.descriptor().displayName);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    buildControls();
    layout->addWidget(strip_);

    if (view_ != nullptr) {
        view_->setPresentations(presentations);
        QWidget* inner = view_->widget();
        inner->setParent(this);
        layout->addWidget(inner, 1);

        view_->setActionSink([this](const QString& name, const QJsonObject& payload,
                                    quint64 revision) { dispatch(name, payload, revision); });

        // Flow-specific controls live in the same strip, so the shared shell
        // does not need to know what they are.
        for (QWidget* control : view_->auxiliaryControls()) {
            strip_->addWidget(control);
        }
    }

    connect(&session_, &application::SessionController::stateChanged, this,
            &ComparisonShell::refresh);
    connect(
        &session_, &application::SessionController::savingChanged, this,
        [this](bool saving, bool unsaved) {
            savingState_->setText(saving ? tr("Saving…") : unsaved ? tr("Unsaved") : tr("Saved"));
        });
    connect(&session_, &application::SessionController::errorOccurred, this,
            &ComparisonShell::errorOccurred);

    refresh(session_.state(), session_.summary());
}

ComparisonShell::~ComparisonShell() = default;

void ComparisonShell::buildControls() {
    strip_ = new QToolBar(this);
    strip_->setObjectName(QStringLiteral("comparisonControls"));
    strip_->setMovable(false);
    // Fullscreen hides browser chrome but keeps this compact strip reachable.
    strip_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    counts_ = new QLabel(this);
    counts_->setObjectName(QStringLiteral("comparisonCounts"));
    strip_->addWidget(counts_);
    strip_->addSeparator();

    undoAction_ = session_.undoStack()->createUndoAction(this, tr("Undo"));
    undoAction_->setObjectName(QStringLiteral("comparisonUndo"));
    undoAction_->setShortcuts(QKeySequence::Undo);
    strip_->addAction(undoAction_);

    redoAction_ = session_.undoStack()->createRedoAction(this, tr("Redo"));
    redoAction_->setObjectName(QStringLiteral("comparisonRedo"));
    redoAction_->setShortcuts(QKeySequence::Redo);
    strip_->addAction(redoAction_);

    strip_->addSeparator();

    finishAction_ = strip_->addAction(tr("Finish"), this, &ComparisonShell::finish);
    finishAction_->setObjectName(QStringLiteral("comparisonFinish"));
    finishAction_->setToolTip(tr("Apply the eliminations made so far as deletion marks."));

    QAction* pauseAction = strip_->addAction(tr("Pause"), this, &ComparisonShell::pause);
    pauseAction->setObjectName(QStringLiteral("comparisonPause"));
    pauseAction->setToolTip(tr("Save this comparison and return to the browser."));

    QAction* discardAction = strip_->addAction(tr("Discard"), this, &ComparisonShell::discard);
    discardAction->setObjectName(QStringLiteral("comparisonDiscard"));
    discardAction->setToolTip(tr("Throw this comparison away. Existing deletion marks are kept."));

    strip_->addSeparator();

    fullscreenAction_ =
        strip_->addAction(tr("Fullscreen"), this, &ComparisonShell::toggleFullscreen);
    fullscreenAction_->setObjectName(QStringLiteral("comparisonFullscreen"));
    fullscreenAction_->setCheckable(true);
    fullscreenAction_->setShortcut(QKeySequence::FullScreen);

    savingState_ = new QLabel(tr("Saved"), this);
    savingState_->setObjectName(QStringLiteral("comparisonSavingState"));
    strip_->addWidget(savingState_);
}

void ComparisonShell::setPresentations(const AssetPresentationMap& presentations) {
    if (view_ != nullptr) {
        view_->setPresentations(presentations);
        view_->setState(session_.state(), session_.summary());
    }
}

void ComparisonShell::refresh(const domain::FlowState& state, const domain::FlowSummary& summary) {
    if (view_ != nullptr) {
        view_->setState(state, summary);
    }
    counts_->setText(tr("%1 remaining · %2 eliminated · %3")
                         .arg(summary.remaining.size())
                         .arg(summary.draftRejected.size())
                         .arg(summary.statusText));
    finishAction_->setEnabled(summary.canFinish);
}

void ComparisonShell::dispatch(const QString& name, const QJsonObject& payload, quint64 revision) {
    QString error;
    if (!session_.dispatchAt(name, payload, revision, &error)) {
        // A rejected action is normal (a late click, a repeated key). It is
        // reported, never applied.
        Q_EMIT errorOccurred(error);
    }
}

void ComparisonShell::toggleFullscreen() {
    if (fullscreen_) {
        fullscreen_ = false;
        showNormal();
        if (!restoreGeometry_.isEmpty()) {
            restoreGeometry(restoreGeometry_);
        }
    } else {
        restoreGeometry_ = saveGeometry();
        fullscreen_ = true;
        showFullScreen();
    }
    fullscreenAction_->setChecked(fullscreen_);
    if (view_ != nullptr) {
        view_->setFullscreenPresentation(fullscreen_);
    }
    // Keyboard focus must survive the transition.
    if (view_ != nullptr) {
        view_->widget()->setFocus(Qt::OtherFocusReason);
    }
}

void ComparisonShell::finish() {
    QString error;
    if (!session_.finish(&error)) {
        Q_EMIT errorOccurred(error);
        return;
    }
    // Emitted before close(): with WA_DeleteOnClose the shell may be scheduled
    // for deletion by the time close() returns.
    Q_EMIT closed(true);
    closingProgrammatically_ = true;
    close();
}

void ComparisonShell::pause() {
    QString error;
    if (!session_.pause(&error)) {
        Q_EMIT errorOccurred(error);
        return;
    }
    // Emitted before close(): with WA_DeleteOnClose the shell may be scheduled
    // for deletion by the time close() returns.
    Q_EMIT closed(false);
    closingProgrammatically_ = true;
    close();
}

void ComparisonShell::discard() {
    const int answer = QMessageBox::question(
        this, tr("Discard comparison"),
        tr("Throw away the %1 elimination(s) made in this comparison?\n\nDeletion marks from "
           "earlier comparisons are kept.")
            .arg(session_.summary().draftRejected.size()),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return;
    }

    QString error;
    if (!session_.discard(&error)) {
        Q_EMIT errorOccurred(error);
        return;
    }
    // Emitted before close(): with WA_DeleteOnClose the shell may be scheduled
    // for deletion by the time close() returns.
    Q_EMIT closed(false);
    closingProgrammatically_ = true;
    close();
}

void ComparisonShell::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        // Escape leaves fullscreen first; only otherwise does it pause.
        if (fullscreen_) {
            toggleFullscreen();
        } else {
            pause();
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ComparisonShell::closeEvent(QCloseEvent* event) {
    if (closingProgrammatically_ || !session_.isActive()) {
        event->accept();
        return;
    }
    // Closing the window is never a silent apply or discard: it pauses.
    QString error;
    if (!session_.pause(&error)) {
        Q_EMIT errorOccurred(error);
        event->ignore();
        return;
    }
    event->accept();
    Q_EMIT closed(false);
}

} // namespace cullfinch::ui
