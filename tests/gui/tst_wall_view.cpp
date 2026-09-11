// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuiFixture.h"

#include <cullfinch/flows/wall/WallFlow.h>
#include <cullfinch/views/wall/WallView.h>

#include <QAction>
#include <QCheckBox>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTest>

#include <tuple>

using namespace cullfinch;
using cullfinch::guitests::GuiFixture;

class TestWallView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void showsATileForEverySelectedPhoto();
    void tilesFitUncroppedInsideTheSurface();
    void clickingATileRemovesOnlyThatPhoto();
    void repeatClicksInTheVacatedRegionDoNotRejectTheNextPhoto();
    void keyRepeatDoesNotRejectASequence();
    void fixedPositionsKeepSurvivorsInPlaceUntilCompact();
    void undoReinstatesThePhotoAndItsPosition();
    void anEmptyWallStillOffersUndoAndFinish();
    void keyboardOnlyCullingWorks();
    void resizingBetweenPressAndReleaseDoesNotMisfire();

private:
    void startWallOn(int count);

    std::unique_ptr<GuiFixture> fixture_;
    views::wall::WallView* view_ = nullptr;
    ui::ComparisonShell* shell_ = nullptr;
};

void TestWallView::initTestCase() {
    guitests::requirePlatform(qEnvironmentVariable("CULLFINCH_EXPECTED_PLATFORM"));
}

void TestWallView::init() {
    fixture_ = std::make_unique<GuiFixture>();
    QString error;
    QVERIFY2(fixture_->initialise(&error), qPrintable(error));

    // Mixed orientations, so the fit policy is exercised for real.
    for (int index = 1; index <= 6; ++index) {
        const QSize size = (index % 2 == 0) ? QSize(200, 300) : QSize(300, 200);
        fixture_->collection().addJpeg(QStringLiteral("IMG_%1.JPG").arg(index), size);
        fixture_->collection().addRaw(QStringLiteral("IMG_%1.RAF").arg(index));
    }

    QVERIFY(fixture_->showWindow() != nullptr);
    guitests::settleWindow(fixture_->window());
    QVERIFY(fixture_->openCollection(6));
}

void TestWallView::cleanup() {
    view_ = nullptr;
    shell_ = nullptr;
    fixture_.reset();
}

void TestWallView::startWallOn(int count) {
    QList<domain::AssetId> ids;
    for (int row = 0; row < count; ++row) {
        ids.append(fixture_->window()->model()->idForRow(row));
    }
    fixture_->window()->selectAssets(ids);
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("image-wall")));

    shell_ = fixture_->window()->activeShell();
    QVERIFY(shell_ != nullptr);
    shell_->resize(1000, 700);
    guitests::settleWindow(shell_);
    view_ = dynamic_cast<views::wall::WallView*>(shell_->view());
    QVERIFY(view_ != nullptr);

    std::ignore = GuiFixture::waitFor([this, count]() {
        int ready = 0;
        for (const domain::AssetId& id : view_->surface()->order()) {
            ui::ImageCanvas* tile = view_->surface()->tileFor(id);
            if (tile != nullptr && tile->isReady()) {
                ++ready;
            }
        }
        return ready == count;
    });
}

void TestWallView::showsATileForEverySelectedPhoto() {
    startWallOn(6);
    // Every selected candidate is on the wall; nothing is paginated away.
    QCOMPARE(view_->surface()->order().size(), 6);
    for (const domain::AssetId& id : view_->surface()->order()) {
        QVERIFY(view_->surface()->tileFor(id) != nullptr);
        QVERIFY(view_->surface()->tileFor(id)->isVisible());
    }
}

void TestWallView::tilesFitUncroppedInsideTheSurface() {
    startWallOn(6);
    const QRect bounds = view_->surface()->rect();

    for (const domain::AssetId& id : view_->surface()->order()) {
        ui::ImageCanvas* tile = view_->surface()->tileFor(id);
        QVERIFY2(bounds.contains(tile->geometry()), "a tile escaped the wall surface");

        // The whole image is drawn inside the tile: never cropped.
        const QRectF drawn = tile->imageRect();
        QVERIFY(drawn.width() <= tile->width() + 0.5);
        QVERIFY(drawn.height() <= tile->height() + 0.5);
    }

    // Mixed portrait and landscape both fit.
    QSet<QString> shapes;
    for (const domain::AssetId& id : view_->surface()->order()) {
        const QRectF drawn = view_->surface()->tileFor(id)->imageRect();
        shapes.insert(drawn.width() > drawn.height() ? QStringLiteral("landscape")
                                                     : QStringLiteral("portrait"));
    }
    QCOMPARE(shapes.size(), 2);
}

void TestWallView::clickingATileRemovesOnlyThatPhoto() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    const domain::AssetId victim = view_->surface()->order().at(2);
    ui::ImageCanvas* tile = view_->surface()->tileFor(victim);
    QVERIFY(tile != nullptr);
    QTest::mouseClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 5; }));
    QCOMPARE(session.summary().draftRejected, QList<domain::AssetId>{victim});
    QVERIFY(view_->surface()->tileFor(victim) == nullptr ||
            !view_->surface()->order().contains(victim));
}

void TestWallView::repeatClicksInTheVacatedRegionDoNotRejectTheNextPhoto() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    const domain::AssetId victim = view_->surface()->order().at(0);
    ui::ImageCanvas* tile = view_->surface()->tileFor(victim);
    const QRect region = tile->geometry();
    QTest::mouseClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 5; }));

    // Click the same coordinates again immediately: whichever tile reflowed
    // into that region must not be rejected by the leftover gesture.
    QWidget* landed = view_->surface()->childAt(region.center());
    if (landed != nullptr) {
        QTest::mouseClick(landed, Qt::LeftButton, Qt::NoModifier,
                          landed->mapFrom(view_->surface(), region.center()));
        QCoreApplication::processEvents();
    }
    QCOMPARE(session.summary().draftRejected.size(), 1);
}

void TestWallView::keyRepeatDoesNotRejectASequence() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    ui::ImageCanvas* tile = view_->surface()->tileFor(view_->surface()->order().first());
    tile->setFocus();

    // Holding Delete generates auto-repeat events, which must be swallowed.
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(tile, &press);
    for (int repeat = 0; repeat < 5; ++repeat) {
        QKeyEvent autoRepeat(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier, 0, 0, 0, QString(),
                             true);
        QCoreApplication::sendEvent(tile, &autoRepeat);
    }

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 1);
}

void TestWallView::fixedPositionsKeepSurvivorsInPlaceUntilCompact() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    auto* fixed = shell_->findChild<QCheckBox*>(QStringLiteral("wallFixedPositions"));
    auto* compact = shell_->findChild<QPushButton*>(QStringLiteral("wallCompact"));
    QVERIFY(fixed != nullptr);
    QVERIFY(compact != nullptr);
    QVERIFY(!compact->isEnabled());

    fixed->setChecked(true);
    QVERIFY(GuiFixture::waitFor([&]() {
        return flows::wall::WallFlow::layoutMode(session.state()) ==
               flows::wall::LayoutMode::FixedPositions;
    }));

    const domain::AssetId neighbour = view_->surface()->order().at(3);
    const QRect before = view_->surface()->tileFor(neighbour)->geometry();

    ui::ImageCanvas* victim = view_->surface()->tileFor(view_->surface()->order().at(1));
    QTest::mouseClick(victim, Qt::LeftButton, Qt::NoModifier, victim->rect().center());

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));
    QCoreApplication::processEvents();

    // Spatial memory is preserved: the survivor did not move.
    QCOMPARE(view_->surface()->tileFor(neighbour)->geometry(), before);
    QVERIFY(compact->isEnabled());

    QTest::mouseClick(compact, Qt::LeftButton);
    QVERIFY(GuiFixture::waitFor(
        [&]() { return !flows::wall::WallFlow::hasPlaceholders(session.state()); }));
}

void TestWallView::undoReinstatesThePhotoAndItsPosition() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    const QList<domain::AssetId> before = view_->surface()->order();
    ui::ImageCanvas* victim = view_->surface()->tileFor(before.at(2));
    const QRect victimGeometry = victim->geometry();
    QTest::mouseClick(victim, Qt::LeftButton, Qt::NoModifier, victim->rect().center());

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 5; }));

    auto* undo = shell_->findChild<QAction*>(QStringLiteral("comparisonUndo"));
    QVERIFY(undo != nullptr);
    undo->trigger();

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 6; }));
    QCoreApplication::processEvents();

    // The same photo, in the same deterministic position.
    QCOMPARE(view_->surface()->order(), before);
    QCOMPARE(view_->surface()->tileFor(before.at(2))->geometry(), victimGeometry);
}

void TestWallView::anEmptyWallStillOffersUndoAndFinish() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    while (!session.summary().remaining.isEmpty()) {
        QString error;
        QJsonObject payload;
        payload.insert(QStringLiteral("assetId"), session.summary().remaining.first().toString());
        QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    }
    QCoreApplication::processEvents();

    auto* empty = shell_->findChild<QLabel*>(QStringLiteral("wallEmptyLabel"));
    QVERIFY(empty != nullptr);
    QVERIFY(empty->isVisible());

    // Zero survivors is a legitimate outcome, and Undo still works there.
    auto* finish = shell_->findChild<QAction*>(QStringLiteral("comparisonFinish"));
    auto* undo = shell_->findChild<QAction*>(QStringLiteral("comparisonUndo"));
    QVERIFY(finish != nullptr);
    QVERIFY(undo != nullptr);
    QVERIFY(finish->isEnabled());
    QVERIFY(undo->isEnabled());

    undo->trigger();
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 1; }));
}

void TestWallView::keyboardOnlyCullingWorks() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    // Eliminate a tile without ever using the mouse.
    ui::ImageCanvas* tile = view_->surface()->tileFor(view_->surface()->order().at(1));
    QVERIFY(tile != nullptr);

    // Every tile takes keyboard focus, which is what makes tab navigation
    // between them possible. Whether the compositor actually grants focus is
    // not asserted here: a headless compositor can have no input seat, and the
    // widget's own keyboard handling is what this suite owns.
    QCOMPARE(tile->focusPolicy(), Qt::StrongFocus);
    tile->setFocus(Qt::TabFocusReason);

    // Accessible names carry the identity and state, not colour alone.
    QVERIFY(!tile->accessibleName().isEmpty());

    QTest::keyClick(tile, Qt::Key_Delete);
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 5; }));

    auto* finish = shell_->findChild<QAction*>(QStringLiteral("comparisonFinish"));
    finish->trigger();
    QVERIFY(
        GuiFixture::waitFor([&]() { return fixture_->root().collection().rejectedCount() == 1; }));
}

void TestWallView::resizingBetweenPressAndReleaseDoesNotMisfire() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    ui::ImageCanvas* tile = view_->surface()->tileFor(view_->surface()->order().at(4));
    const QPoint centre = tile->rect().center();

    QTest::mousePress(tile, Qt::LeftButton, Qt::NoModifier, centre);
    shell_->resize(760, 520);
    QCoreApplication::processEvents();
    // Releasing far outside the tile is not a decision about it.
    QTest::mouseRelease(tile, Qt::LeftButton, Qt::NoModifier, centre + QPoint(4000, 4000));

    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 0);
}

QTEST_MAIN(TestWallView)
#include "tst_wall_view.moc"
