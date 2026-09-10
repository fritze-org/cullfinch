// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/SessionController.h>
#include <cullfinch/ui/FlowView.h>

#include <QAction>
#include <QLabel>
#include <QPointer>
#include <QToolBar>
#include <QWidget>

#include <memory>

namespace cullfinch::ui {

/// The shared shell every flow runs inside.
///
/// It supplies remaining and rejected counts, Undo, Redo, Finish, Pause and
/// Discard, plus an optional fullscreen presentation. Escape leaves fullscreen
/// first; otherwise it pauses and returns to the browser. It never silently
/// applies or discards decisions.
class ComparisonShell : public QWidget {
    Q_OBJECT

public:
    /// Presentations are a constructor argument on purpose: the shell renders
    /// the flow's current state as soon as it is built, and a view handed an
    /// empty map at that moment would latch onto empty panes and never refill
    /// them.
    ComparisonShell(application::SessionController& session, std::unique_ptr<IFlowView> view,
                    const AssetPresentationMap& presentations, QWidget* parent = nullptr);
    ~ComparisonShell() override;

    ComparisonShell(const ComparisonShell&) = delete;
    ComparisonShell& operator=(const ComparisonShell&) = delete;
    ComparisonShell(ComparisonShell&&) = delete;
    ComparisonShell& operator=(ComparisonShell&&) = delete;

    void setPresentations(const AssetPresentationMap& presentations);

    [[nodiscard]] IFlowView* view() { return view_.get(); }

signals:
    /// The session ended, either applied (Finish) or not (Pause, Discard).
    void closed(bool applied);
    void errorOccurred(const QString& message);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildControls();
    void refresh(const domain::FlowState& state, const domain::FlowSummary& summary);
    void dispatch(const QString& name, const QJsonObject& payload, quint64 revision);
    void toggleFullscreen();
    void finish();
    void pause();
    void discard();

    application::SessionController& session_;
    std::unique_ptr<IFlowView> view_;

    QToolBar* strip_ = nullptr;
    QLabel* counts_ = nullptr;
    QLabel* savingState_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* finishAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;

    /// Size and window state only. Wayland does not let a client place its own
    /// window -- QWindow::setPosition is documented as unsupported there -- so
    /// restoring an exact desktop coordinate is not something to promise. The
    /// comparison state and internal focus are what actually matter across the
    /// transition, and those are preserved.
    QSize normalSize_;
    bool fullscreen_ = false;
    bool closingProgrammatically_ = false;
};

} // namespace cullfinch::ui
