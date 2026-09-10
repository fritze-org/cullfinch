// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/CollectionController.h>
#include <cullfinch/application/DispositionController.h>
#include <cullfinch/application/FlowRegistry.h>
#include <cullfinch/application/ImageService.h>
#include <cullfinch/application/OperationController.h>
#include <cullfinch/application/SessionController.h>
#include <cullfinch/ui/AssetListModel.h>
#include <cullfinch/ui/ComparisonShell.h>
#include <cullfinch/ui/FlowView.h>

#include <QAction>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QSortFilterProxyModel>

#include <functional>
#include <utility>

namespace cullfinch::ui {

/// Everything the browser needs, wired by the composition root.
struct AppContext {
    application::IAssetRepository& repository;
    application::CollectionController& collection;
    application::SessionController& session;
    application::DispositionController& dispositions;
    application::OperationController& operations;
    application::FlowRegistry& flows;
    FlowViewRegistry& flowViews;
    application::IImageService& images;
};

/// Filters over the collection, including the diagnostic view for RAW-only and
/// ambiguous groups.
class AssetFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT

public:
    enum class Mode { All, NeedsAttention, Rejected, Comparable };
    Q_ENUM(Mode)

    explicit AssetFilterProxy(QObject* parent = nullptr);
    void setMode(Mode mode);
    [[nodiscard]] Mode mode() const { return mode_; }

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex& sourceParent) const override;

private:
    Mode mode_ = Mode::All;
};

/// Directory path, thumbnail grid, sort and filter controls, comparison menu
/// and a visible rejection count.
class BrowserWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit BrowserWindow(const AppContext& context, QWidget* parent = nullptr);
    ~BrowserWindow() override;

    BrowserWindow(const BrowserWindow&) = delete;
    BrowserWindow& operator=(const BrowserWindow&) = delete;
    BrowserWindow(BrowserWindow&&) = delete;
    BrowserWindow& operator=(BrowserWindow&&) = delete;

    /// Open a directory without going through the native picker. Used by the
    /// command-line directory argument and by GUI tests.
    bool openDirectory(const QString& path);

    /// Start a flow on the current selection. Exposed so GUI tests drive the
    /// same path the menu does.
    bool startFlow(const QString& flowId);

    /// Collection-local staging root for file operations. Defaults to a hidden
    /// directory beside the photos; tests point it somewhere disposable.
    void setStagingRoot(const QString& path);

    /// Resume or discard a saved draft without a modal prompt. Used by tests
    /// and by the restart flow.
    bool resumeSession(const application::StoredSession& stored, QString* error);

    /// What to do about a draft found when a collection is opened.
    enum class ResumeChoice { Resume, Discard, Leave };

    /// Supplies the answer when a saved draft is found. Asking the user is a
    /// composition concern, not the browser's: with no prompt installed the
    /// draft is simply left alone, which is the safe default and keeps
    /// automated runs from blocking on a dialog nobody can dismiss.
    using ResumePrompt = std::function<ResumeChoice(const application::StoredSession&)>;
    void setResumePrompt(ResumePrompt prompt) { resumePrompt_ = std::move(prompt); }

    [[nodiscard]] QList<domain::AssetId> selectedAssetIds() const;
    void selectAssets(const QList<domain::AssetId>& ids);

    [[nodiscard]] AssetListModel* model() { return model_; }
    [[nodiscard]] QListView* grid() { return grid_; }
    [[nodiscard]] ComparisonShell* activeShell() { return shell_.data(); }

signals:
    void statusMessage(const QString& message);
    /// A recoverable failure worth telling the user about. The browser puts
    /// it in the status bar; whether it also warrants a modal dialog is the
    /// composition root's decision, which is what keeps automated runs from
    /// blocking on a dialog nobody can dismiss.
    void errorOccurred(const QString& message);

protected:
    void changeEvent(QEvent* event) override;

private:
    void buildMenus();
    void buildCentralWidget();
    void refreshAssets();
    void refreshStatus();
    void chooseDirectory();
    void unmarkSelection();
    void reviewFileOperations();
    void offerResume();
    void reportError(const QString& message);

    AppContext context_;
    AssetListModel* model_ = nullptr;
    AssetFilterProxy* proxy_ = nullptr;
    QListView* grid_ = nullptr;
    QLabel* pathLabel_ = nullptr;
    QLabel* rejectionLabel_ = nullptr;
    QLabel* diagnosticsLabel_ = nullptr;
    QMenu* compareMenu_ = nullptr;
    QAction* recursiveAction_ = nullptr;
    QAction* reviewAction_ = nullptr;
    QAction* unmarkAction_ = nullptr;
    QPointer<ComparisonShell> shell_;
    QString stagingRoot_;
    ResumePrompt resumePrompt_;
    QMetaObject::Connection sessionEndedConnection_;
    quint64 presentationGeneration_ = 0;
};

} // namespace cullfinch::ui
