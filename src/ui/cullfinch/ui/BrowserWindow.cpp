// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/BrowserWindow.h>

#include <cullfinch/domain/SelectionSnapshot.h>
#include <cullfinch/ui/ReviewDialog.h>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

namespace cullfinch::ui {

AssetFilterProxy::AssetFilterProxy(QObject* parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
}

void AssetFilterProxy::setMode(Mode mode) {
    if (mode_ == mode) {
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    // invalidateFilter() is deprecated from 6.13; the begin/end pair lets the
    // proxy keep its persistent indexes valid across the change, which matters
    // because the browser's selection is held through those indexes.
    beginFilterChange();
    mode_ = mode;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    mode_ = mode;
    invalidateFilter();
#endif
}

bool AssetFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    if (!index.isValid()) {
        return false;
    }
    const auto state = static_cast<domain::PairingState>(
        sourceModel()->data(index, AssetListModel::PairingStateRole).toInt());
    const auto disposition = static_cast<domain::Disposition>(
        sourceModel()->data(index, AssetListModel::DispositionRole).toInt());

    switch (mode_) {
    case Mode::All:
        return true;
    case Mode::NeedsAttention:
        // The diagnostic view: RAW-only, ambiguous and stale groups.
        return state == domain::PairingState::RawOnly || state == domain::PairingState::Ambiguous ||
               state == domain::PairingState::Stale ||
               !sourceModel()->data(index, AssetListModel::OperableRole).toBool();
    case Mode::Rejected:
        return disposition == domain::Disposition::Reject;
    case Mode::Comparable:
        break;
    }
    return sourceModel()->data(index, AssetListModel::ComparableRole).toBool() &&
           disposition == domain::Disposition::Neutral;
}

BrowserWindow::BrowserWindow(const AppContext& context, QWidget* parent)
    : QMainWindow(parent), context_(context) {
    setObjectName(QStringLiteral("browserWindow"));
    setWindowTitle(tr("cullfinch"));
    resize(1100, 760);

    buildCentralWidget();
    buildMenus();

    connect(&context_.collection, &application::CollectionController::assetsChanged, this,
            &BrowserWindow::refreshAssets);
    connect(&context_.collection, &application::CollectionController::errorOccurred, this,
            &BrowserWindow::reportError);
    connect(&context_.collection, &application::CollectionController::scanStateChanged, this,
            [this](bool scanning) {
                statusBar()->showMessage(scanning ? tr("Scanning…") : tr("Ready"), 4000);
            });
    connect(&context_.collection, &application::CollectionController::diagnosticsChanged, this,
            [this](const QStringList& diagnostics) {
                diagnosticsLabel_->setText(
                    diagnostics.isEmpty() ? QString()
                                          : tr("%1 association issue(s)").arg(diagnostics.size()));
                diagnosticsLabel_->setToolTip(diagnostics.join(QLatin1Char('\n')));
            });

    connect(&context_.dispositions, &application::DispositionController::dispositionsChanged, this,
            [this](const QList<domain::AssetId>& affected, quint64 revision) {
                context_.collection.applyDispositionChange(affected, revision);
                refreshStatus();
            });
    connect(&context_.dispositions, &application::DispositionController::errorOccurred, this,
            &BrowserWindow::reportError);
    connect(&context_.dispositions, &application::DispositionController::markingEnabledChanged,
            this, [this](bool enabled) { unmarkAction_->setEnabled(enabled); });

    connect(&context_.session, &application::SessionController::errorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 6000); });

    refreshStatus();
}

BrowserWindow::~BrowserWindow() = default;

void BrowserWindow::buildCentralWidget() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    pathLabel_ = new QLabel(tr("No directory open"), central);
    pathLabel_->setObjectName(QStringLiteral("collectionPath"));
    pathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(pathLabel_);

    model_ = new AssetListModel(context_.images, this);
    proxy_ = new AssetFilterProxy(this);
    proxy_->setSourceModel(model_);

    grid_ = new QListView(central);
    grid_->setObjectName(QStringLiteral("assetGrid"));
    grid_->setModel(proxy_);
    grid_->setViewMode(QListView::IconMode);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(true);
    grid_->setIconSize(QSize(160, 160));
    grid_->setGridSize(QSize(196, 210));
    grid_->setSpacing(6);
    // Standard platform selection: Shift for ranges, Ctrl/Command to toggle,
    // and the platform Select All shortcut.
    grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    grid_->setSelectionBehavior(QAbstractItemView::SelectItems);
    layout->addWidget(grid_, 1);

    connect(grid_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this]() { refreshStatus(); });

    setCentralWidget(central);

    auto* status = new QStatusBar(this);
    setStatusBar(status);
    rejectionLabel_ = new QLabel(status);
    rejectionLabel_->setObjectName(QStringLiteral("rejectionCount"));
    status->addPermanentWidget(rejectionLabel_);
    diagnosticsLabel_ = new QLabel(status);
    diagnosticsLabel_->setObjectName(QStringLiteral("diagnosticsCount"));
    status->addPermanentWidget(diagnosticsLabel_);
}

void BrowserWindow::buildMenus() {
    QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));

    QAction* openAction =
        fileMenu->addAction(tr("&Open Directory…"), this, &BrowserWindow::chooseDirectory);
    openAction->setObjectName(QStringLiteral("actionOpenDirectory"));
    openAction->setShortcut(QKeySequence::Open);

    QAction* refreshAction =
        fileMenu->addAction(tr("&Refresh"), this, [this]() { context_.collection.refresh(); });
    refreshAction->setObjectName(QStringLiteral("actionRefresh"));
    refreshAction->setShortcut(QKeySequence::Refresh);

    recursiveAction_ = fileMenu->addAction(tr("Include &Subdirectories"));
    recursiveAction_->setObjectName(QStringLiteral("actionRecursive"));
    recursiveAction_->setCheckable(true);
    connect(recursiveAction_, &QAction::toggled, this, [this](bool) {
        if (!context_.collection.rootPath().isEmpty()) {
            openDirectory(context_.collection.rootPath());
        }
    });

    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(tr("&Quit"), qApp, &QApplication::quit);
    quitAction->setObjectName(QStringLiteral("actionQuit"));
    quitAction->setShortcut(QKeySequence::Quit);

    QMenu* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->setObjectName(QStringLiteral("editMenu"));

    // The browser's Undo reverses a completed comparison's marks. It never
    // secretly reopens that comparison's view.
    QAction* undoAction = context_.dispositions.undoStack()->createUndoAction(this, tr("&Undo"));
    undoAction->setObjectName(QStringLiteral("actionUndoMarks"));
    undoAction->setShortcuts(QKeySequence::Undo);
    editMenu->addAction(undoAction);

    QAction* redoAction = context_.dispositions.undoStack()->createRedoAction(this, tr("&Redo"));
    redoAction->setObjectName(QStringLiteral("actionRedoMarks"));
    redoAction->setShortcuts(QKeySequence::Redo);
    editMenu->addAction(redoAction);

    editMenu->addSeparator();
    QAction* selectAllAction = editMenu->addAction(tr("Select &All"), this, [this]() {
        grid_->selectAll();
        grid_->setFocus();
    });
    selectAllAction->setObjectName(QStringLiteral("actionSelectAll"));
    selectAllAction->setShortcut(QKeySequence::SelectAll);

    unmarkAction_ =
        editMenu->addAction(tr("Un&mark Selection"), this, &BrowserWindow::unmarkSelection);
    unmarkAction_->setObjectName(QStringLiteral("actionUnmark"));

    compareMenu_ = menuBar()->addMenu(tr("&Compare"));
    compareMenu_->setObjectName(QStringLiteral("compareMenu"));
    // Built from the registry: a new flow appears here without a code change
    // in the browser.
    for (const domain::FlowDescriptor& descriptor : context_.flows.descriptors()) {
        QAction* action = compareMenu_->addAction(descriptor.displayName, this,
                                                  [this, id = descriptor.id]() { startFlow(id); });
        action->setObjectName(QStringLiteral("actionFlow_") + descriptor.id);
        action->setToolTip(descriptor.description);
    }

    QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));

    const auto addFilter = [this, viewMenu](const QString& text, AssetFilterProxy::Mode mode,
                                            const QString& name) {
        QAction* action =
            viewMenu->addAction(text, this, [this, mode]() { proxy_->setMode(mode); });
        action->setObjectName(name);
        action->setCheckable(true);
        return action;
    };
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    group
        ->addAction(addFilter(tr("&All photos"), AssetFilterProxy::Mode::All,
                              QStringLiteral("actionFilterAll")))
        ->setChecked(true);
    group->addAction(addFilter(tr("&Needs attention"), AssetFilterProxy::Mode::NeedsAttention,
                               QStringLiteral("actionFilterAttention")));
    group->addAction(addFilter(tr("&Rejected"), AssetFilterProxy::Mode::Rejected,
                               QStringLiteral("actionFilterRejected")));

    QMenu* operationsMenu = menuBar()->addMenu(tr("&Operations"));
    operationsMenu->setObjectName(QStringLiteral("operationsMenu"));
    reviewAction_ = operationsMenu->addAction(tr("Review &File Operations…"), this,
                                              &BrowserWindow::reviewFileOperations);
    reviewAction_->setObjectName(QStringLiteral("actionReviewOperations"));
}

bool BrowserWindow::openDirectory(const QString& path) {
    QString error;
    if (!context_.collection.open(path, recursiveAction_->isChecked(), &error)) {
        reportError(error);
        return false;
    }
    ++presentationGeneration_;
    pathLabel_->setText(path);
    setWindowTitle(tr("cullfinch — %1").arg(path));
    offerResume();
    return true;
}

void BrowserWindow::chooseDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("Open a directory of photos"),
                                                           context_.collection.rootPath());
    if (!path.isEmpty()) {
        openDirectory(path);
    }
}

void BrowserWindow::refreshAssets() {
    model_->setAssets(context_.collection.assets(), presentationGeneration_);
    refreshStatus();
}

void BrowserWindow::refreshStatus() {
    const int rejected = context_.collection.rejectedCount();
    rejectionLabel_->setText(tr("%1 marked for deletion").arg(rejected));
    reviewAction_->setEnabled(rejected > 0);

    const QList<domain::AssetId> selected = selectedAssetIds();
    bool anyRejectedSelected = false;
    for (const domain::AssetId& id : selected) {
        const domain::PhotoAsset* asset = context_.collection.asset(id);
        if (asset != nullptr && asset->disposition == domain::Disposition::Reject) {
            anyRejectedSelected = true;
            break;
        }
    }
    unmarkAction_->setEnabled(anyRejectedSelected && context_.dispositions.isMarkingEnabled());
}

QList<domain::AssetId> BrowserWindow::selectedAssetIds() const {
    QList<domain::AssetId> ids;
    const QModelIndexList selected = grid_->selectionModel()->selectedIndexes();
    // Selection order follows the view, which is the frozen order a flow gets.
    QList<QModelIndex> sourceIndexes;
    sourceIndexes.reserve(selected.size());
    for (const QModelIndex& index : selected) {
        sourceIndexes.append(proxy_->mapToSource(index));
    }
    std::sort(sourceIndexes.begin(), sourceIndexes.end(),
              [](const QModelIndex& lhs, const QModelIndex& rhs) { return lhs.row() < rhs.row(); });
    for (const QModelIndex& index : sourceIndexes) {
        ids.append(model_->idForRow(index.row()));
    }
    return ids;
}

void BrowserWindow::selectAssets(const QList<domain::AssetId>& ids) {
    QItemSelection selection;
    for (const domain::AssetId& id : ids) {
        const int row = model_->rowForId(id);
        if (row < 0) {
            continue;
        }
        const QModelIndex mapped = proxy_->mapFromSource(model_->index(row, 0));
        if (mapped.isValid()) {
            selection.select(mapped, mapped);
        }
    }
    grid_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
}

bool BrowserWindow::startFlow(const QString& flowId) {
    if (shell_ != nullptr) {
        shell_->raise();
        return false;
    }

    const QList<domain::AssetId> selectedIds = selectedAssetIds();
    const domain::PhotoAssetList selected = model_->assetsForIds(selectedIds);
    const domain::SelectionEligibility eligibility = domain::selectEligible(
        context_.collection.collectionId(), context_.collection.revision(), selected);

    if (eligibility.hasExclusions()) {
        // Launching reports what was excluded and why, rather than quietly
        // dropping photos from the comparison.
        QStringList lines;
        for (const domain::ExcludedAsset& excluded : eligibility.excluded) {
            lines.append(tr("%1 — %2").arg(excluded.displayName, excluded.reason));
        }
        statusBar()->showMessage(
            tr("%1 selected photo(s) were left out").arg(eligibility.excluded.size()), 8000);
        statusBar()->setToolTip(lines.join(QLatin1Char('\n')));
    }

    QString error;
    if (!context_.session.start(flowId, eligibility.snapshot, domain::FlowOptions{}, &error)) {
        reportError(error);
        return false;
    }

    std::unique_ptr<IFlowView> view = context_.flowViews.create(flowId, context_.images);
    if (view == nullptr) {
        context_.session.discard(&error);
        reportError(tr("No view is registered for comparison flow '%1'.").arg(flowId));
        return false;
    }

    AssetPresentationMap presentations;
    for (const domain::PhotoAsset& asset :
         model_->assetsForIds(eligibility.snapshot.orderedAssetIds)) {
        presentations.insert(asset.id, AssetPresentation::from(asset));
    }

    shell_ = new ComparisonShell(context_.session, std::move(view), presentations, this);
    shell_->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(shell_.data(), &ComparisonShell::errorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 6000); });
    connect(shell_.data(), &ComparisonShell::closed, this, [this](bool) { refreshStatus(); });

    // After finishing, the survivors become the browser selection, so a wall
    // pass can be followed by versus without coupling the two flows. The
    // connection is single-shot so repeated comparisons do not stack up.
    disconnect(sessionEndedConnection_);
    sessionEndedConnection_ = connect(
        &context_.session, &application::SessionController::sessionEnded, this,
        [this, snapshot = eligibility.snapshot](const QString&, bool applied) {
            if (!applied) {
                return;
            }
            QList<domain::AssetId> survivors;
            for (const domain::AssetId& id : snapshot.orderedAssetIds) {
                const domain::PhotoAsset* asset = context_.collection.asset(id);
                if (asset != nullptr && asset->disposition == domain::Disposition::Neutral) {
                    survivors.append(id);
                }
            }
            selectAssets(survivors);
        },
        Qt::SingleShotConnection);

    shell_->show();
    shell_->raise();
    shell_->activateWindow();
    return true;
}

void BrowserWindow::unmarkSelection() {
    QList<domain::AssetId> ids;
    for (const domain::AssetId& id : selectedAssetIds()) {
        const domain::PhotoAsset* asset = context_.collection.asset(id);
        if (asset != nullptr && asset->disposition == domain::Disposition::Reject) {
            ids.append(id);
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    QString error;
    if (!context_.dispositions.unmark(ids, &error)) {
        reportError(error);
    }
}

void BrowserWindow::reviewFileOperations() {
    const domain::PhotoAssetList rejected = context_.collection.rejectedAssets();
    if (rejected.isEmpty()) {
        statusBar()->showMessage(tr("Nothing is marked for deletion."), 4000);
        return;
    }

    const QString stagingRoot = stagingRoot_.isEmpty() ? context_.collection.rootPath() +
                                                             QStringLiteral("/.cullfinch-staging")
                                                       : stagingRoot_;

    const domain::PlanningResult planning = context_.operations.review(
        context_.collection.collectionId(), context_.collection.revision(), stagingRoot, rejected);

    ReviewDialog dialog(planning, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.executionRequested()) {
        return;
    }

    QString error;
    if (!context_.operations.execute(dialog.plan(), context_.collection.assets(), &error)) {
        reportError(error);
        context_.collection.refresh();
        return;
    }

    statusBar()->showMessage(tr("Moved %1 photos to Trash.").arg(dialog.plan().logicalPhotoCount()),
                             6000);
    context_.collection.refresh();
}

void BrowserWindow::setStagingRoot(const QString& path) {
    stagingRoot_ = path;
}

bool BrowserWindow::resumeSession(const application::StoredSession& stored, QString* error) {
    if (!context_.session.resume(stored, context_.collection.assets(), error)) {
        return false;
    }

    std::unique_ptr<IFlowView> view =
        context_.flowViews.create(stored.draft.flowId, context_.images);
    if (view == nullptr) {
        QString discardError;
        context_.session.pause(&discardError);
        if (error != nullptr) {
            *error = tr("No view is registered for comparison flow '%1'.").arg(stored.draft.flowId);
        }
        return false;
    }

    AssetPresentationMap presentations;
    for (const domain::PhotoAsset& asset : model_->assetsForIds(stored.snapshot.orderedAssetIds)) {
        presentations.insert(asset.id, AssetPresentation::from(asset));
    }

    shell_ = new ComparisonShell(context_.session, std::move(view), presentations, this);
    shell_->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(shell_.data(), &ComparisonShell::errorOccurred, this,
            [this](const QString& message) { statusBar()->showMessage(message, 6000); });
    connect(shell_.data(), &ComparisonShell::closed, this, [this](bool) { refreshStatus(); });
    shell_->show();
    return true;
}

void BrowserWindow::offerResume() {
    if (!resumePrompt_) {
        return; // Nobody to ask; the draft stays saved and untouched.
    }

    QString error;
    const QList<application::StoredSession> saved =
        context_.repository.resumableSessions(context_.collection.collectionId(), &error);
    if (saved.isEmpty()) {
        return;
    }

    const application::StoredSession& latest = saved.first();
    switch (resumePrompt_(latest)) {
    case ResumeChoice::Resume: {
        QString resumeError;
        if (!resumeSession(latest, &resumeError)) {
            // An unknown or newer state version is reported, never reinterpreted.
            reportError(resumeError);
        }
        break;
    }
    case ResumeChoice::Discard: {
        QString deleteError;
        if (!context_.repository.deleteSession(latest.id, &deleteError)) {
            reportError(deleteError);
        }
        break;
    }
    case ResumeChoice::Leave:
        break;
    }
}

void BrowserWindow::reportError(const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    statusBar()->showMessage(message, 8000);
    statusBar()->setToolTip(message);
    Q_EMIT errorOccurred(message);
}

void BrowserWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && isActiveWindow() &&
        !context_.collection.rootPath().isEmpty()) {
        // A watcher is only a hint, so refresh on activation too.
        context_.collection.refresh();
    }
}

} // namespace cullfinch::ui
