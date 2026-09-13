// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuiFixture.h"

#include <cullfinch/flows/wall/WallFlow.h>
#include <cullfinch/views/wall/WallView.h>

#include <QAction>
#include <QCheckBox>
#include <QGuiApplication>
#include <QImage>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSet>
#include <QStyleHints>
#include <QTest>

#include <tuple>

using namespace cullfinch;
using cullfinch::guitests::GuiFixture;

namespace {

/// The size every wall test lays out at. Tile geometry is a function of the
/// surface size, so readings taken at different sizes are not comparable --
/// which is why the tests that compare two of them settle on this size again
/// before the second one.
const QSize kWallWindowSize(1000, 700);

} // namespace

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
    void anEliminatedTileStaysInItsCellMarkedEliminated();
    void aPlaceholderWithNoCandidateStillHoldsItsCell();
    void aTileCreatedBeforeItsPresentationPicksItUp();
    void undoReinstatesThePhotoAndItsPosition();
    void anEmptyWallStillOffersUndoAndFinish();
    void anEmptiedFixedWallStillShowsWhatWasEliminated();
    void keyboardOnlyCullingWorks();
    void resizingBetweenPressAndReleaseDoesNotMisfire();
    void aGestureIsBoundToTheLayoutItWasPressedOn();
    void aCancelledPointerGestureDoesNotTaintTheKeyboard();

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
    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));
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

    // Once the double-click interval has passed, the same coordinates are an
    // ordinary target again: the suppression is about a leftover gesture,
    // not about the region.
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
    landed = view_->surface()->childAt(region.center());
    if (landed != nullptr) {
        QTest::mouseClick(landed, Qt::LeftButton, Qt::NoModifier,
                          landed->mapFrom(view_->surface(), region.center()));
        QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 2; }));
    }
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

    // Spatial memory is preserved: the survivor did not move. Both readings
    // have to be taken at the same surface size, so a window manager that
    // revised it in between is undone first; it would move every tile.
    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));
    QCOMPARE(view_->surface()->tileFor(neighbour)->geometry(), before);
    QVERIFY(compact->isEnabled());

    QTest::mouseClick(compact, Qt::LeftButton);
    QVERIFY(GuiFixture::waitFor(
        [&]() { return !flows::wall::WallFlow::hasPlaceholders(session.state()); }));
}

void TestWallView::anEliminatedTileStaysInItsCellMarkedEliminated() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    auto* fixed = shell_->findChild<QCheckBox*>(QStringLiteral("wallFixedPositions"));
    auto* compact = shell_->findChild<QPushButton*>(QStringLiteral("wallCompact"));
    QVERIFY(fixed != nullptr);
    QVERIFY(compact != nullptr);

    fixed->setChecked(true);
    QVERIFY(GuiFixture::waitFor([&]() {
        return flows::wall::WallFlow::layoutMode(session.state()) ==
               flows::wall::LayoutMode::FixedPositions;
    }));

    // Both renderings have to be taken at the same surface size, for the reason
    // the other fixed-position case states.
    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));
    const domain::AssetId victim = view_->surface()->order().at(2);
    ui::ImageCanvas* tile = view_->surface()->tileFor(victim);
    if (tile == nullptr) {
        QFAIL("the wall has no tile for the photo it is about to eliminate");
    }
    QVERIFY(!tile->isRejected());
    const QRect cell = tile->geometry();
    const QImage surviving = tile->grab().toImage();
    const QString survivingName = tile->accessibleName();

    QTest::mouseClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().remaining.size() == 5; }));
    QCoreApplication::processEvents();

    // The placeholder is a *rejected* placeholder: the photo stays in its cell,
    // marked as eliminated, so the user can see what the cell is being held
    // for. It is no longer a survivor.
    QVERIFY2(view_->surface()->tileFor(victim) == tile, "the eliminated tile was replaced");
    QVERIFY(tile->isRejected());
    QVERIFY(tile->isVisible());
    QVERIFY(!view_->surface()->order().contains(victim));

    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));
    QCOMPARE(tile->geometry(), cell);
    QVERIFY2(tile->grab().toImage() != surviving, "an eliminated tile paints identically");
    // State reaches assistive technology as text, not as colour alone.
    QVERIFY2(tile->accessibleName() != survivingName,
             "an eliminated tile has the accessible name of a survivor");

    // Its tile is reachable, so both a click and a key can still land on it.
    // One photo is one decision.
    QTest::mouseClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());
    QTest::keyClick(tile, Qt::Key_Delete);
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 1);

    // Compacting takes the cell, and the tile with it.
    QTest::mouseClick(compact, Qt::LeftButton);
    QVERIFY(GuiFixture::waitFor([&]() { return view_->surface()->tileFor(victim) == nullptr; }));
}

void TestWallView::aPlaceholderWithNoCandidateStillHoldsItsCell() {
    startWallOn(6);
    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));

    // A wall paused by a build that did not keep the placeholder's candidate
    // restores with an anonymous cell. There is no photo to put in it, and it
    // still has to hold its position, or the survivors move -- which is the one
    // thing fixed-position mode promises. The surface is driven directly
    // because no flow in this build can produce that state any more.
    ui::AssetPresentationMap presentations;
    QList<flows::wall::WallSlot> positions;
    QHash<domain::AssetId, QRect> before;
    for (const domain::AssetId& id : view_->surface()->order()) {
        ui::ImageCanvas* placed = view_->surface()->tileFor(id);
        if (placed == nullptr) {
            QFAIL("a candidate on the wall has no tile");
        }
        presentations.insert(id, placed->presentation());
        positions.append(flows::wall::WallSlot{id, false});
        before.insert(id, placed->geometry());
    }

    const domain::AssetId anonymous = positions.at(3).id;
    positions[3] = flows::wall::WallSlot{};
    view_->surface()->setCandidates(positions, presentations,
                                    view_->surface()->layoutRevision() + 1);
    QCoreApplication::processEvents();

    QVERIFY(view_->surface()->tileFor(anonymous) == nullptr);
    QCOMPARE(view_->surface()->order().size(), 5);
    for (const domain::AssetId& id : view_->surface()->order()) {
        ui::ImageCanvas* survivor = view_->surface()->tileFor(id);
        if (survivor == nullptr) {
            QFAIL("a survivor lost its tile");
        }
        QCOMPARE(survivor->geometry(), before.value(id));
    }
}

void TestWallView::aTileCreatedBeforeItsPresentationPicksItUp() {
    startWallOn(5);
    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));

    ui::AssetPresentationMap presentations;
    QList<flows::wall::WallSlot> positions;
    for (const domain::AssetId& id : view_->surface()->order()) {
        ui::ImageCanvas* placed = view_->surface()->tileFor(id);
        if (placed == nullptr) {
            QFAIL("a candidate on the wall has no tile");
        }
        presentations.insert(id, placed->presentation());
        positions.append(flows::wall::WallSlot{id, false});
    }

    // State can reach the surface before the presentations for it do. The tile
    // is created anyway, so the grid is right, and it has no photo to show yet.
    const domain::AssetId late = fixture_->window()->model()->idForRow(5);
    QVERIFY(late.isValid());
    positions.append(flows::wall::WallSlot{late, false});
    view_->surface()->setCandidates(positions, presentations,
                                    view_->surface()->layoutRevision() + 1);
    QCoreApplication::processEvents();

    ui::ImageCanvas* tile = view_->surface()->tileFor(late);
    if (tile == nullptr) {
        QFAIL("the wall created no tile for the candidate it was handed");
    }
    QVERIFY(!tile->presentation().previewMemberId.isValid());
    QVERIFY(!tile->isReady());

    // The same state again leaves it as it is: there is still nothing to fill
    // it in with, and a tile is never re-presented with what it already has.
    view_->surface()->setCandidates(positions, presentations,
                                    view_->surface()->layoutRevision() + 1);
    QCoreApplication::processEvents();
    QVERIFY2(view_->surface()->tileFor(late) == tile, "the waiting tile was replaced");
    QVERIFY(!tile->presentation().previewMemberId.isValid());

    // When they arrive, the tile picks its photo up rather than staying blank.
    for (const domain::PhotoAsset& asset : fixture_->root().collection().assets()) {
        if (asset.id == late) {
            presentations.insert(late, ui::AssetPresentation::from(asset));
        }
    }
    QVERIFY(presentations.value(late).previewMemberId.isValid());
    view_->surface()->setCandidates(positions, presentations,
                                    view_->surface()->layoutRevision() + 1);

    QVERIFY(GuiFixture::waitFor([tile]() { return tile->isReady(); }));
    QCOMPARE(tile->presentation().id, late);
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

    // The same photo, in the same deterministic position -- read at the size
    // the first reading was taken at, for the reason above.
    QVERIFY(guitests::settleWindowSize(shell_, kWallWindowSize));
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

void TestWallView::anEmptiedFixedWallStillShowsWhatWasEliminated() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    auto* fixed = shell_->findChild<QCheckBox*>(QStringLiteral("wallFixedPositions"));
    auto* compact = shell_->findChild<QPushButton*>(QStringLiteral("wallCompact"));
    if (fixed == nullptr || compact == nullptr) {
        QFAIL("the wall view has no layout controls");
    }
    fixed->setChecked(true);
    QVERIFY(GuiFixture::waitFor([&]() {
        return flows::wall::WallFlow::layoutMode(session.state()) ==
               flows::wall::LayoutMode::FixedPositions;
    }));

    const QList<domain::AssetId> culled = view_->surface()->order();
    while (!session.summary().remaining.isEmpty()) {
        QString error;
        QJsonObject payload;
        payload.insert(QStringLiteral("assetId"), session.summary().remaining.first().toString());
        QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    }
    QCoreApplication::processEvents();

    // No survivors, but every cell is still held, and in fixed-position mode a
    // held cell is a photo marked as eliminated. Hiding the wall here would
    // take the spatial record away exactly where Undo needs it.
    QVERIFY(view_->surface()->isVisible());
    QVERIFY(view_->surface()->order().isEmpty());
    for (const domain::AssetId& id : culled) {
        ui::ImageCanvas* tile = view_->surface()->tileFor(id);
        if (tile == nullptr) {
            QFAIL("an eliminated photo lost the cell that was holding it");
        }
        QVERIFY(tile->isRejected());
    }

    auto* empty = shell_->findChild<QLabel*>(QStringLiteral("wallEmptyLabel"));
    if (empty == nullptr) {
        QFAIL("the wall view has no empty-wall label");
    }
    QVERIFY(empty->isVisible());
    QVERIFY(compact->isEnabled());

    // Compacting an emptied wall leaves nothing to show, and the label carries
    // the state on its own.
    QTest::mouseClick(compact, Qt::LeftButton);
    QVERIFY(GuiFixture::waitFor(
        [&]() { return !flows::wall::WallFlow::hasPlaceholders(session.state()); }));
    QCoreApplication::processEvents();
    QVERIFY(!view_->surface()->isVisible());
    QVERIFY(empty->isVisible());
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

void TestWallView::aGestureIsBoundToTheLayoutItWasPressedOn() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    // Fixed positions, so the layout change below moves nothing and resizes
    // nothing: the only thing that changes under the pressed pointer is the
    // layout revision.
    auto* fixed = shell_->findChild<QCheckBox*>(QStringLiteral("wallFixedPositions"));
    QVERIFY(fixed != nullptr);
    fixed->setChecked(true);
    QVERIFY(GuiFixture::waitFor([&]() {
        return flows::wall::WallFlow::layoutMode(session.state()) ==
               flows::wall::LayoutMode::FixedPositions;
    }));

    const domain::AssetId pressed = view_->surface()->order().at(4);
    ui::ImageCanvas* tile = view_->surface()->tileFor(pressed);
    QVERIFY(tile != nullptr);
    const QPoint centre = tile->rect().center();
    QTest::mousePress(tile, Qt::LeftButton, Qt::NoModifier, centre);

    // Another decision lands while the button is down (a key, a linked
    // input, a late event) and advances the layout revision.
    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), view_->surface()->order().at(0).toString());
    QString error;
    QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 1);
    QVERIFY(view_->surface()->tileFor(pressed) == tile);

    // The release reports against the layout it was pressed on, which the
    // host now rejects as stale: a gesture never decides a second photo on
    // a layout it did not start on.
    QTest::mouseRelease(tile, Qt::LeftButton, Qt::NoModifier, centre);
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 1);
    QVERIFY(session.summary().remaining.contains(pressed));
}

void TestWallView::aCancelledPointerGestureDoesNotTaintTheKeyboard() {
    startWallOn(6);
    application::SessionController& session = fixture_->root().session();

    auto* fixed = shell_->findChild<QCheckBox*>(QStringLiteral("wallFixedPositions"));
    QVERIFY(fixed != nullptr);
    fixed->setChecked(true);
    QVERIFY(GuiFixture::waitFor([&]() {
        return flows::wall::WallFlow::layoutMode(session.state()) ==
               flows::wall::LayoutMode::FixedPositions;
    }));

    // A press that is abandoned outside the tile decides nothing...
    const domain::AssetId pressed = view_->surface()->order().at(4);
    ui::ImageCanvas* tile = view_->surface()->tileFor(pressed);
    QVERIFY(tile != nullptr);
    QTest::mousePress(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());

    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), view_->surface()->order().at(0).toString());
    QString error;
    QVERIFY2(session.dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));
    QCoreApplication::processEvents();

    QTest::mouseRelease(tile, Qt::LeftButton, Qt::NoModifier,
                        tile->rect().center() + QPoint(4000, 4000));
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 1);

    // ...and the stale press must not be mistaken for the key that follows:
    // Delete acts on the layout as it is now, and goes through.
    tile->setFocus();
    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
    QCoreApplication::sendEvent(tile, &del);
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 2; }));
    QVERIFY(!session.summary().remaining.contains(pressed));
}

QTEST_MAIN(TestWallView)
#include "tst_wall_view.moc"
