// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuiFixture.h"

#include <cullfinch/domain/SelectionSnapshot.h>
#include <cullfinch/ui/ReviewDialog.h>

#include <QAbstractItemModelTester>
#include <QAction>
#include <QItemSelectionModel>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace cullfinch;
using cullfinch::guitests::GuiFixture;

class TestBrowserWindow : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void showsOneTileForEachJpegRawPair();
    void supportsRangeAndDiscontiguousSelection();
    void supportsTheSelectAllShortcut();
    void reportsTheRejectionCount();
    void exposesTheComparisonMenuFromTheRegistry();
    void refusesToStartAFlowWithNothingSelected();
    void wallFlowEndToEndAppliesMarksAndSelectsSurvivors();
    void unmarkRestoresEligibility();
    void reviewCountsPhysicalFilesForTheWholeGroup();
    void aSecondInstanceOpensTheCollectionReadOnly();
    void closingTheBrowserPausesAnActiveComparison();
    void openingAnotherDirectoryPausesAnActiveComparison();
    void theListModelSatisfiesTheModelTester();
    void tilesKeepTheirFullSizeWhileThumbnailsAreStillDecoding();

private:
    std::unique_ptr<GuiFixture> fixture_;
};

void TestBrowserWindow::initTestCase() {
    guitests::requirePlatform(qEnvironmentVariable("CULLFINCH_EXPECTED_PLATFORM"));
}

void TestBrowserWindow::init() {
    fixture_ = std::make_unique<GuiFixture>();
    QString error;
    QVERIFY2(fixture_->initialise(&error), qPrintable(error));

    for (int index = 1; index <= 6; ++index) {
        fixture_->collection().addJpeg(QStringLiteral("IMG_%1.JPG").arg(index));
        fixture_->collection().addRaw(QStringLiteral("IMG_%1.RAF").arg(index));
    }

    QVERIFY(fixture_->showWindow() != nullptr);
    guitests::settleWindow(fixture_->window());
    QVERIFY2(fixture_->openCollection(6), "the scan did not publish six photos");
}

void TestBrowserWindow::cleanup() {
    fixture_.reset();
}

void TestBrowserWindow::showsOneTileForEachJpegRawPair() {
    // Twelve files, six photos: the group, not the file, is the visible unit.
    QCOMPARE(fixture_->window()->model()->rowCount(), 6);

    for (int row = 0; row < 6; ++row) {
        const QModelIndex index = fixture_->window()->model()->index(row, 0);
        QCOMPARE(index.data(ui::AssetListModel::RawCountRole).toInt(), 1);
        QCOMPARE(index.data(ui::AssetListModel::MemberCountRole).toInt(), 2);
        QVERIFY(index.data(ui::AssetListModel::ComparableRole).toBool());
        // The RAW badge is text, not colour alone.
        QVERIFY(index.data(Qt::DisplayRole).toString().contains(QStringLiteral("RAW")));
    }
}

void TestBrowserWindow::supportsRangeAndDiscontiguousSelection() {
    QListView* grid = fixture_->window()->grid();
    QAbstractItemModel* model = grid->model();

    // Shift-click a range.
    grid->setCurrentIndex(model->index(1, 0));
    grid->selectionModel()->select(QItemSelection(model->index(1, 0), model->index(3, 0)),
                                   QItemSelectionModel::ClearAndSelect);
    QCOMPARE(fixture_->window()->selectedAssetIds().size(), 3);

    // Ctrl/Command-click adds a discontiguous item.
    grid->selectionModel()->select(model->index(5, 0), QItemSelectionModel::Select);
    const QList<domain::AssetId> selected = fixture_->window()->selectedAssetIds();
    QCOMPARE(selected.size(), 4);

    // Selection order follows the view, which is what a flow freezes.
    QCOMPARE(selected.first(), fixture_->window()->model()->idForRow(1));
    QCOMPARE(selected.last(), fixture_->window()->model()->idForRow(5));
}

void TestBrowserWindow::supportsTheSelectAllShortcut() {
    QAction* selectAll = fixture_->window()->findChild<QAction*>(QStringLiteral("actionSelectAll"));
    QVERIFY(selectAll != nullptr);
    QCOMPARE(selectAll->shortcut(), QKeySequence(QKeySequence::SelectAll));

    selectAll->trigger();
    QCOMPARE(fixture_->window()->selectedAssetIds().size(), 6);
}

void TestBrowserWindow::reportsTheRejectionCount() {
    auto* label = fixture_->window()->findChild<QLabel*>(QStringLiteral("rejectionCount"));
    QVERIFY(label != nullptr);
    QVERIFY(label->text().contains(QStringLiteral("0")));
}

void TestBrowserWindow::exposesTheComparisonMenuFromTheRegistry() {
    // The menu is built from the registry, so both shipped flows appear
    // without the browser naming them.
    QVERIFY(fixture_->window()->findChild<QAction*>(QStringLiteral("actionFlow_versus-tree")) !=
            nullptr);
    QVERIFY(fixture_->window()->findChild<QAction*>(QStringLiteral("actionFlow_image-wall")) !=
            nullptr);
}

void TestBrowserWindow::refusesToStartAFlowWithNothingSelected() {
    fixture_->window()->grid()->clearSelection();
    QVERIFY(!fixture_->window()->startFlow(QStringLiteral("versus-tree")));
    QVERIFY(fixture_->window()->activeShell() == nullptr);
}

void TestBrowserWindow::wallFlowEndToEndAppliesMarksAndSelectsSurvivors() {
    fixture_->window()->selectAssets({fixture_->window()->model()->idForRow(0),
                                      fixture_->window()->model()->idForRow(1),
                                      fixture_->window()->model()->idForRow(2)});
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("image-wall")));

    ui::ComparisonShell* shell = fixture_->window()->activeShell();
    QVERIFY(shell != nullptr);
    guitests::settleWindow(shell);

    application::SessionController& session = fixture_->root().session();
    const domain::AssetId victim = session.summary().remaining.at(1);

    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), victim.toString());
    QString error;
    QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    QCOMPARE(session.summary().remaining.size(), 2);

    // No mark exists until Finish: eliminating only changes the draft.
    QCOMPARE(fixture_->root().collection().rejectedCount(), 0);

    auto* finish = shell->findChild<QAction*>(QStringLiteral("comparisonFinish"));
    QVERIFY(finish != nullptr);
    QVERIFY(finish->isEnabled());
    finish->trigger();

    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 1; }));

    const domain::PhotoAsset* marked = fixture_->root().collection().asset(victim);
    QVERIFY(marked != nullptr);
    QCOMPARE(marked->disposition, domain::Disposition::Reject);

    // After finishing, the survivors are the browser selection.
    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->window()->selectedAssetIds().size() == 2; }));
    QVERIFY(!fixture_->window()->selectedAssetIds().contains(victim));
}

void TestBrowserWindow::unmarkRestoresEligibility() {
    const domain::AssetId target = fixture_->window()->model()->idForRow(0);
    QString error;
    QVERIFY2(
        fixture_->root().dispositions().applyRejections({target}, QStringLiteral("test"), &error),
        qPrintable(error));
    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 1; }));

    // A rejected photo is excluded from a new comparison until it is unmarked.
    fixture_->window()->selectAssets({target});
    QVERIFY(!fixture_->window()->startFlow(QStringLiteral("versus-tree")));

    auto* unmark = fixture_->window()->findChild<QAction*>(QStringLiteral("actionUnmark"));
    QVERIFY(unmark != nullptr);
    fixture_->window()->selectAssets({target});
    QVERIFY(unmark->isEnabled());
    unmark->trigger();

    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 0; }));
}

void TestBrowserWindow::reviewCountsPhysicalFilesForTheWholeGroup() {
    const QList<domain::AssetId> targets{fixture_->window()->model()->idForRow(0),
                                         fixture_->window()->model()->idForRow(1)};
    QString error;
    QVERIFY(
        fixture_->root().dispositions().applyRejections(targets, QStringLiteral("test"), &error));
    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 2; }));

    const domain::PlanningResult planning = fixture_->root().operations().review(
        fixture_->root().collection().collectionId(), fixture_->root().collection().revision(),
        fixture_->collection().filePath(QStringLiteral(".cullfinch-staging")),
        fixture_->root().collection().rejectedAssets());

    // Two photos, four physical files: every RAW travels with its JPG.
    QCOMPARE(planning.plan.logicalPhotoCount(), 2);
    QCOMPARE(planning.plan.physicalFileCount(), 4);
    QVERIFY(!planning.hasBlockers());

    ui::ReviewDialog dialog(planning);
    auto* summary = dialog.findChild<QLabel*>(QStringLiteral("reviewSummary"));
    QVERIFY(summary != nullptr);
    QVERIFY(summary->text().contains(QStringLiteral("2 photos")));
    QVERIFY(summary->text().contains(QStringLiteral("4 files")));
    // Executing is a separate, explicit action.
    QVERIFY(!dialog.executionRequested());
}

void TestBrowserWindow::aSecondInstanceOpensTheCollectionReadOnly() {
    // The fixture's window holds the writer lock. A second composition
    // against the same application data root is exactly a second launch.
    app::CompositionRoot::Options options;
    options.dataDirectory = fixture_->dataDirectory();
    options.cacheDirectory = fixture_->cacheDirectory();
    options.trashAdapter = &fixture_->trash();
    app::CompositionRoot second(options);
    QString error;
    QVERIFY2(second.initialise(&error), qPrintable(error));
    std::unique_ptr<ui::BrowserWindow> window(second.createBrowserWindow());

    QSignalSpy readOnly(&second.collection(), &application::CollectionController::readOnlyChanged);
    QVERIFY2(window->openDirectory(fixture_->collection().path()),
             "a held lock downgrades the open; it does not refuse it");
    QVERIFY(second.collection().isReadOnly());
    QVERIFY(!second.collection().readOnlyReason().isEmpty());
    QCOMPARE(readOnly.size(), 1);
    QVERIFY(readOnly.first().at(0).toBool());

    // The stored inventory is browsable without a scan of our own...
    QCOMPARE(window->model()->rowCount(), 6);
    const auto* indicator = window->findChild<QLabel*>(QStringLiteral("readOnlyIndicator"));
    QVERIFY(indicator != nullptr && !indicator->text().isEmpty());

    // ...but nothing that writes is accepted: no draft, no mark, no operation.
    const domain::AssetId first = window->model()->idForRow(0);
    window->selectAssets({first, window->model()->idForRow(1)});
    QVERIFY(!window->startFlow(QStringLiteral("image-wall")));
    QVERIFY(window->activeShell() == nullptr);
    QVERIFY(!second.dispositions().applyRejections({first}, QStringLiteral("test"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!second.dispositions().unmark({first}, &error));
    QVERIFY(!second.session().resume(application::StoredSession{}, second.collection().assets(),
                                     &error));
    QVERIFY2(error.contains(QStringLiteral("read-only")), qPrintable(error));
    // Callers that do not want the reason still get the refusal.
    QVERIFY(!second.dispositions().applyRejections({first}, QStringLiteral("test"), nullptr));
    QVERIFY(!second.dispositions().unmark({first}, nullptr));
    QVERIFY(!second.session().start(
        QStringLiteral("image-wall"),
        domain::SelectionSnapshot::freeze(second.collection().collectionId(), 1,
                                          second.collection().assets()),
        domain::FlowOptions{}, nullptr));
    // A refresh is a rescan, and a rescan is a write: refused too.
    second.collection().refresh();
    QVERIFY(!second.collection().isScanning());
    auto* review = window->findChild<QAction*>(QStringLiteral("actionReviewOperations"));
    QVERIFY(review != nullptr && !review->isEnabled());
    // The action is disabled, but the guard behind it holds on its own: a
    // shortcut or a stale menu that reaches it is refused with a reason.
    QSignalSpy refused(window.get(), &ui::BrowserWindow::errorOccurred);
    review->setEnabled(true);
    review->trigger();
    QCOMPARE(refused.size(), 1);
    QVERIFY(refused.first().at(0).toString().contains(QStringLiteral("read-only")));
    const auto* compare = window->findChild<QMenu*>(QStringLiteral("compareMenu"));
    QVERIFY(compare != nullptr && !compare->isEnabled());

    // Closing clears the state, and once the first window lets go, reopening
    // takes the lock and scans.
    second.collection().close();
    QVERIFY(!second.collection().isReadOnly());
    QCOMPARE(readOnly.size(), 2);
    second.collection().refresh(); // Nothing open: nothing to scan.
    QVERIFY(!second.collection().isScanning());
    fixture_->root().collection().close();
    QVERIFY(window->openDirectory(fixture_->collection().path()));
    QVERIFY(!second.collection().isReadOnly());
    QVERIFY(compare != nullptr && compare->isEnabled());
    QVERIFY(GuiFixture::waitFor([&]() { return !second.collection().isScanning(); }));
    QCOMPARE(window->model()->rowCount(), 6);
}

void TestBrowserWindow::closingTheBrowserPausesAnActiveComparison() {
    fixture_->window()->selectAssets({fixture_->window()->model()->idForRow(0),
                                      fixture_->window()->model()->idForRow(1),
                                      fixture_->window()->model()->idForRow(2)});
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("image-wall")));
    application::SessionController& session = fixture_->root().session();

    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), session.summary().remaining.at(1).toString());
    QString error;
    QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    // The autosave is coalesced; the write has not happened yet.
    QVERIFY(session.hasUnsavedChanges());

    // Closing the browser takes the comparison with it. That is a pause with
    // the draft written first -- never a silent apply, discard, or a lost
    // autosave.
    QVERIFY(fixture_->window()->close());
    QVERIFY(!session.isActive());
    QCOMPARE(fixture_->root().collection().rejectedCount(), 0);

    const QList<application::StoredSession> saved = fixture_->root().repository().resumableSessions(
        fixture_->root().collection().collectionId(), &error);
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().lifecycle, application::SessionLifecycle::Paused);
    QCOMPARE(saved.first().draftRejected.size(), 1);
}

void TestBrowserWindow::openingAnotherDirectoryPausesAnActiveComparison() {
    fixture_->window()->selectAssets({fixture_->window()->model()->idForRow(0),
                                      fixture_->window()->model()->idForRow(1),
                                      fixture_->window()->model()->idForRow(2)});
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("image-wall")));
    application::SessionController& session = fixture_->root().session();
    const domain::CollectionId original = fixture_->root().collection().collectionId();

    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), session.summary().remaining.at(1).toString());
    QString error;
    QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    QVERIFY(session.hasUnsavedChanges());

    // The comparison belongs to the collection it started on. Opening a
    // different directory pauses it, draft written, before the collection
    // changes underneath it; a comparison must never outlive its collection
    // or carry on against one that may have opened read-only.
    const QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    QVERIFY(fixture_->window()->openDirectory(elsewhere.path()));
    QVERIFY(!session.isActive());
    QVERIFY(fixture_->root().collection().collectionId().toString() != original.toString());

    const QList<application::StoredSession> saved =
        fixture_->root().repository().resumableSessions(original, &error);
    QCOMPARE(saved.size(), 1);
    QCOMPARE(saved.first().lifecycle, application::SessionLifecycle::Paused);
    QCOMPARE(saved.first().draftRejected.size(), 1);
}

void TestBrowserWindow::theListModelSatisfiesTheModelTester() {
    // Qt's own contract checker for QAbstractItemModel: every signal the
    // model emits while assets, marks and generations change is validated
    // against what the views are entitled to assume.
    QAbstractItemModelTester tester(fixture_->window()->model(),
                                    QAbstractItemModelTester::FailureReportingMode::QtTest);

    const domain::AssetId target = fixture_->window()->model()->idForRow(0);
    QString error;
    QVERIFY(
        fixture_->root().dispositions().applyRejections({target}, QStringLiteral("test"), &error));
    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 1; }));
    QVERIFY(fixture_->root().dispositions().unmark({target}, &error));
    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 0; }));

    // A rescan republishes the whole set.
    fixture_->collection().addJpeg(QStringLiteral("IMG_7.JPG"));
    QVERIFY(fixture_->openCollection(7));
    QCOMPARE(fixture_->window()->model()->rowCount(), 7);
}

void TestBrowserWindow::tilesKeepTheirFullSizeWhileThumbnailsAreStillDecoding() {
    // Decoding is asynchronous, so a tile is laid out before its thumbnail
    // exists. The grid's uniform-item-size optimisation samples one size hint
    // at layout time and never asks again, and it repaints exactly the tile
    // rectangle that sampling produced -- so a hint that only grows once the
    // image arrives leaves every photo drawn as a sliver a few scanlines tall
    // until something forces a full relayout.
    QListView* grid = fixture_->window()->grid();
    const QModelIndex first = grid->model()->index(0, 0);
    QVERIFY(first.isValid());

    const QRect beforeDecode = grid->visualRect(first);
    QVERIFY2(beforeDecode.height() >= grid->iconSize().height(),
             qPrintable(QStringLiteral("tile is %1px tall before the thumbnail arrives, too short "
                                       "for a %2px icon")
                            .arg(beforeDecode.height())
                            .arg(grid->iconSize().height())));

    // Asking for the decoration is what the view does when it paints, and what
    // starts the decode.
    QVERIFY(GuiFixture::waitFor([&]() { return first.data(Qt::DecorationRole).isValid(); }));

    // The arriving thumbnail must not move or resize the tile it lands in.
    QCOMPARE(grid->visualRect(first), beforeDecode);
}

QTEST_MAIN(TestBrowserWindow)
#include "tst_browser_window.moc"
