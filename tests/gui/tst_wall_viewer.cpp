// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuiFixture.h"

#include <cullfinch/testsupport/FakeImageService.h>
#include <cullfinch/testsupport/TempCollection.h>
#include <cullfinch/viewer/DirectoryPhotos.h>
#include <cullfinch/viewer/WallViewerWindow.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSet>
#include <QSignalSpy>
#include <QSlider>
#include <QTest>

#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>

using namespace cullfinch;

namespace {

ui::AssetPresentation photoNamed(const QString& name) {
    ui::AssetPresentation presentation;
    presentation.id = domain::AssetId(name);
    presentation.previewMemberId = domain::MemberId(name + QStringLiteral("-jpg"));
    presentation.displayName = name;
    presentation.previewPath = QStringLiteral("/photos/") + name;
    return presentation;
}

/// A directory's worth of photos that exist only as presentations: the image
/// service is a fake, so nothing is ever read from these paths.
viewer::DirectoryPhotos photos(int count) {
    viewer::DirectoryPhotos result;
    for (int index = 1; index <= count; ++index) {
        result.photos.append(photoNamed(QStringLiteral("IMG_%1.JPG").arg(index)));
    }
    return result;
}

template<typename Predicate>
bool waitFor(Predicate predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return predicate();
}

/// Events go straight to the widget: a headless compositor can have no input
/// seat, so synthesising them through the platform would test the session.
void sendClick(QWidget* widget, const QPoint& at) {
    for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, QPointF(at), QPointF(widget->mapToGlobal(at)), Qt::LeftButton,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(widget, &event);
    }
}

void sendKey(QWidget* widget, Qt::Key key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
}

/// Big enough for a few rows of small tiles, small enough that a hundred of
/// them have to scroll.
constexpr QSize kWindowSize(720, 540);

} // namespace

/// The standalone wall: a directory's photos, a tile size, and no decisions.
///
/// Geometry here is the application's own -- the surface inside a scroll area
/// inside the window -- so the window is sized once and only the tiles change.
class TestWallViewer : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void aDirectoryIsGroupedLikeTheBrowserGroupsIt();
    void anUnreadableDirectoryIsReportedNotShownEmpty();
    void anEmptyDirectorySaysSo();
    void aFittedWallShowsEveryPhotoWithoutScrolling();
    void aTileWidthScrollsAndDecodesOnlyWhatIsNearTheScreen();
    void resizingTilesWaitsForTheSizeToSettle();
    void clickingOrDeletingATileRemovesNothing();
    void theControlsChangeTheTileWidth();
    void zoomingKeepsThePhotoUnderThePointerInPlace();
    void escapeBacksOutOneLevelAtATime();

private:
    /// Build and show a viewer, and wait until the size it is laid out at is
    /// the one asked for. Callers check QTest::currentTestFailed() afterwards.
    void show(const viewer::DirectoryPhotos& shown, int tileWidth);
    /// The photos the viewer has asked the image service about, whatever size.
    [[nodiscard]] QSet<domain::MemberId> requestedMembers() const;
    /// Answer every request made so far, so every tile has a preview on screen.
    void deliverEverything();

    std::unique_ptr<testsupport::FakeImageService> images_;
    std::unique_ptr<viewer::WallViewerWindow> window_;
};

void TestWallViewer::initTestCase() {
    if (const QString expected = qEnvironmentVariable("CULLFINCH_EXPECTED_PLATFORM");
        !expected.isEmpty()) {
        QCOMPARE(QGuiApplication::platformName(), expected);
    }
}

void TestWallViewer::cleanup() {
    window_.reset(); // Before the service its tiles hold a reference to.
    images_.reset();
}

void TestWallViewer::show(const viewer::DirectoryPhotos& shown, int tileWidth) {
    images_ = std::make_unique<testsupport::FakeImageService>();
    window_ = std::make_unique<viewer::WallViewerWindow>(*images_, QStringLiteral("/photos"), shown,
                                                         tileWidth);
    window_->show();
    // Tile geometry is read throughout, so the size has to have held: a window
    // manager that reverts it later would move tiles a case already measured.
    QVERIFY(guitests::settleWindowSize(window_.get(), kWindowSize));
}

QSet<domain::MemberId> TestWallViewer::requestedMembers() const {
    QSet<domain::MemberId> members;
    for (const application::ImageRequest& request : images_->requests()) {
        members.insert(request.memberId);
    }
    return members;
}

void TestWallViewer::deliverEverything() {
    for (qsizetype index = 0; index < images_->requests().size(); ++index) {
        images_->succeed(index, QSize(300, 200));
    }
    QCoreApplication::processEvents();
}

void TestWallViewer::aDirectoryIsGroupedLikeTheBrowserGroupsIt() {
    testsupport::TempCollection collection;
    QVERIFY(collection.isValid());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_10.JPG")).isEmpty());
    QVERIFY(!collection.addJpeg(QStringLiteral("IMG_9.JPG")).isEmpty());
    QVERIFY(!collection.addRaw(QStringLiteral("IMG_9.RAF")).isEmpty());
    QVERIFY(!collection.addRaw(QStringLiteral("IMG_2.RAF")).isEmpty());
    QVERIFY(collection.addDirectory(QStringLiteral("nested")));
    QVERIFY(!collection.addJpeg(QStringLiteral("nested/IMG_1.JPG")).isEmpty());

    const viewer::DirectoryPhotos loaded = viewer::loadDirectoryPhotos(collection.path());
    QVERIFY2(loaded.error.isEmpty(), qPrintable(loaded.error));

    // One tile per photo with its RAW, in natural order; the RAW on its own is
    // counted rather than shown; the subdirectory is not entered.
    QCOMPARE(loaded.photos.size(), 2);
    QCOMPARE(loaded.photos.at(0).displayName, QStringLiteral("IMG_9"));
    QCOMPARE(loaded.photos.at(0).rawCount, 1);
    QCOMPARE(loaded.photos.at(1).displayName, QStringLiteral("IMG_10"));
    QCOMPARE(loaded.withoutPreview, 1);

    show(loaded, 0);
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(window_->findChild<QLabel*>(QStringLiteral("viewerHiddenCount")) != nullptr);
}

void TestWallViewer::anUnreadableDirectoryIsReportedNotShownEmpty() {
    const viewer::DirectoryPhotos loaded =
        viewer::loadDirectoryPhotos(QStringLiteral("/nonexistent/cullfinch-wall-test"));
    QVERIFY(!loaded.error.isEmpty());
    QVERIFY(loaded.photos.isEmpty());
}

void TestWallViewer::anEmptyDirectorySaysSo() {
    show(photos(0), 0);
    if (QTest::currentTestFailed()) {
        return;
    }
    auto* empty = window_->findChild<QLabel*>(QStringLiteral("viewerEmptyLabel"));
    QVERIFY(empty != nullptr);
    QVERIFY(!empty->isHidden());
    QVERIFY(window_->scrollArea()->isHidden());
    QVERIFY(images_->requests().isEmpty());
}

void TestWallViewer::aFittedWallShowsEveryPhotoWithoutScrolling() {
    show(photos(6), 0);
    if (QTest::currentTestFailed()) {
        return;
    }
    views::wall::WallSurface* surface = window_->surface();

    // The comparison wall's behaviour: every photo on screen, nothing to
    // scroll to, and every photo decoding.
    QCOMPARE(surface->order().size(), 6);
    QCOMPARE(surface->minimumHeight(), 0);
    QCOMPARE(window_->scrollArea()->verticalScrollBar()->maximum(), 0);
    QCOMPARE(requestedMembers().size(), 6);
    for (const domain::AssetId& id : surface->order()) {
        QVERIFY(!surface->tileFor(id)->isLoadingDeferred());
    }
}

void TestWallViewer::aTileWidthScrollsAndDecodesOnlyWhatIsNearTheScreen() {
    const int count = 120;
    show(photos(count), 128);
    if (QTest::currentTestFailed()) {
        return;
    }
    views::wall::WallSurface* surface = window_->surface();
    QScrollBar* bar = window_->scrollArea()->verticalScrollBar();

    QVERIFY(waitFor([&]() { return bar->maximum() > 0; }));
    QCOMPARE(surface->order().size(), count);
    const ui::ImageCanvas* first = surface->tileFor(surface->order().first());
    QVERIFY(first->width() >= 128);
    QVERIFY(first->x() + first->width() <= window_->scrollArea()->viewport()->width());

    // The top of the grid and a screen below it: not the whole directory.
    const qsizetype nearTheTop = requestedMembers().size();
    QVERIFY(nearTheTop > 0);
    QVERIFY2(nearTheTop < count, "every photo decoded, including those nobody can see");
    const domain::AssetId last = surface->order().constLast();
    QVERIFY(surface->tileFor(last)->isLoadingDeferred());
    QVERIFY(
        !requestedMembers().contains(domain::MemberId(last.toString() + QStringLiteral("-jpg"))));

    // Scrolled to the bottom, the last photos load.
    bar->setValue(bar->maximum());
    QVERIFY(waitFor([&]() {
        return requestedMembers().contains(
            domain::MemberId(last.toString() + QStringLiteral("-jpg")));
    }));
    QVERIFY(!surface->tileFor(last)->isLoadingDeferred());
}

void TestWallViewer::resizingTilesWaitsForTheSizeToSettle() {
    show(photos(12), 128);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    views::wall::WallSurface* surface = window_->surface();
    for (const domain::AssetId& id : surface->order()) {
        QVERIFY(surface->tileFor(id)->preview()->isReady());
    }
    const qsizetype before = images_->requests().size();

    // A slider being dragged: many sizes in quick succession. The previews
    // already on screen are scaled meanwhile; nothing is decoded per step.
    for (int width = 144; width <= 320; width += 16) {
        window_->setTileWidth(width);
        QCoreApplication::processEvents();
    }
    QCOMPARE(images_->requests().size(), before);

    // Once the size holds, each tile still near the screen asks once, at the
    // size it ended up with.
    QVERIFY(waitFor([&]() { return images_->requests().size() > before; }));
    QTest::qWait(400);
    const qsizetype refinements = images_->requests().size() - before;
    QVERIFY2(refinements <= surface->order().size(),
             qPrintable(QStringLiteral("%1 refinements for %2 tiles")
                            .arg(refinements)
                            .arg(surface->order().size())));
    for (qsizetype index = before; index < images_->requests().size(); ++index) {
        const application::ImageRequest& asked = images_->requests().at(index);
        const QString name = asked.memberId.toString().chopped(4); // Without "-jpg".
        const ui::ImageCanvas* tile = surface->tileFor(domain::AssetId(name));
        QVERIFY(tile != nullptr);
        const qreal ratio = tile->devicePixelRatioF();
        QCOMPARE(asked.targetSize, QSize(static_cast<int>(tile->width() * ratio),
                                         static_cast<int>(tile->height() * ratio)));
    }
}

void TestWallViewer::clickingOrDeletingATileRemovesNothing() {
    show(photos(4), 0);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    views::wall::WallSurface* surface = window_->surface();
    const QList<domain::AssetId> before = surface->order();

    ui::ImageCanvas* clicked = surface->tileFor(before.at(0));
    sendClick(clicked, clicked->rect().center());
    ui::ImageCanvas* deleted = surface->tileFor(before.at(1));
    sendKey(deleted, Qt::Key_Delete);
    QCoreApplication::processEvents();

    // A viewer decides nothing: every photo is still there, unmarked.
    QCOMPARE(surface->order(), before);
    for (const domain::AssetId& id : before) {
        QVERIFY(!surface->tileFor(id)->isRejected());
    }
}

void TestWallViewer::theControlsChangeTheTileWidth() {
    show(photos(30), 0);
    if (QTest::currentTestFailed()) {
        return;
    }
    QSignalSpy chosen(window_.get(), &viewer::WallViewerWindow::tileWidthChanged);
    auto* fit = window_->findChild<QAction*>(QStringLiteral("viewerFit"));
    auto* slider = window_->findChild<QSlider*>(QStringLiteral("viewerTileSize"));
    auto* larger = window_->findChild<QAction*>(QStringLiteral("viewerLarger"));
    auto* smaller = window_->findChild<QAction*>(QStringLiteral("viewerSmaller"));
    QVERIFY(fit != nullptr && slider != nullptr && larger != nullptr && smaller != nullptr);
    QVERIFY(fit->isChecked());

    // Moving the slider leaves Fit for that size.
    slider->setValue(300);
    QCOMPARE(window_->tileWidth(), 300);
    QVERIFY(!fit->isChecked());
    QCOMPARE(chosen.size(), 1);
    QCOMPARE(chosen.constLast().at(0).toInt(), 300);

    larger->trigger();
    QVERIFY(window_->tileWidth() > 300);
    QCOMPARE(slider->value(), window_->tileWidth());
    smaller->trigger();
    smaller->trigger();
    QVERIFY(window_->tileWidth() < 300);

    // Both ends of the range hold.
    window_->setTileWidth(1);
    QCOMPARE(window_->tileWidth(), viewer::WallViewerWindow::kMinimumTileWidth);
    QVERIFY(!smaller->isEnabled());
    window_->setTileWidth(100000);
    QCOMPARE(window_->tileWidth(), viewer::WallViewerWindow::kMaximumTileWidth);
    QVERIFY(!larger->isEnabled());

    fit->trigger();
    QCOMPARE(window_->tileWidth(), 0);
    QCOMPARE(chosen.constLast().at(0).toInt(), 0);
    QVERIFY(waitFor([&]() { return window_->surface()->minimumHeight() == 0; }));
}

void TestWallViewer::zoomingKeepsThePhotoUnderThePointerInPlace() {
    show(photos(90), 128);
    if (QTest::currentTestFailed()) {
        return;
    }
    views::wall::WallSurface* surface = window_->surface();
    QScrollArea* scroll = window_->scrollArea();
    QScrollBar* bar = scroll->verticalScrollBar();
    QVERIFY(waitFor([&]() { return bar->maximum() > 0; }));
    bar->setValue(bar->maximum() / 2);
    QCoreApplication::processEvents();

    // Inside the tile nearest the middle of the screen rather than in the
    // spacing between two, so which photo is "under the pointer" is not a
    // matter of rounding.
    const QPoint centre = scroll->viewport()->rect().center();
    QPoint anchor;
    int nearest = std::numeric_limits<int>::max();
    for (const domain::AssetId& id : surface->order()) {
        const QRect cell = surface->tileFor(id)->geometry().translated(surface->pos());
        if (const int distance = (cell.center() - centre).manhattanLength(); distance < nearest) {
            nearest = distance;
            anchor = cell.center() + QPoint(5, 7);
        }
    }
    const auto underAnchor = [&]() -> domain::AssetId {
        const QPoint onSurface = surface->mapFrom(scroll->viewport(), anchor);
        for (const domain::AssetId& id : surface->order()) {
            if (surface->tileFor(id)->geometry().contains(onSurface)) {
                return id;
            }
        }
        return {};
    };
    const domain::AssetId anchored = underAnchor();
    QVERIFY(anchored.isValid());
    const ui::ImageCanvas* tile = surface->tileFor(anchored);
    const double fraction =
        static_cast<double>(surface->mapFrom(scroll->viewport(), anchor).y() - tile->y()) /
        tile->height();

    window_->zoomTiles(2, anchor);
    QVERIFY(window_->tileWidth() > 128);

    // Every tile moved, and the column count changed, so the photo may now be
    // in another column: the grid never scrolls sideways. What is kept is its
    // height on screen -- the same point of it level with the pointer --
    // which is what stops a zoom throwing the photo out of view.
    const auto offsetFromAnchor = [&]() {
        const int levelWithAnchor =
            tile->y() + static_cast<int>(std::lround(fraction * tile->height()));
        return surface->mapTo(scroll->viewport(), QPoint(0, levelWithAnchor)).y() - anchor.y();
    };
    QVERIFY2(
        waitFor([&]() { return std::abs(offsetFromAnchor()) <= 1; }),
        qPrintable(
            QStringLiteral("the photo ended %1 pixels from the pointer").arg(offsetFromAnchor())));
}

void TestWallViewer::escapeBacksOutOneLevelAtATime() {
    show(photos(3), 0);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    ui::ImageCanvas* tile = window_->surface()->tileFor(window_->surface()->order().first());
    tile->setFocus();
    tile->setInspecting(true);

    // Escape on an inspected photo leaves inspection and nothing else. Focus
    // may not be granted without an input seat, so the key goes to the tile
    // and whether it reaches the window is Qt's propagation, not the seat's.
    if (QApplication::focusWidget() == tile) {
        sendKey(tile, Qt::Key_Escape);
        QVERIFY(!tile->isInspecting());
        QVERIFY(window_->isVisible());
    }

    sendKey(window_.get(), Qt::Key_Escape);
    QVERIFY(!window_->isVisible());
}

QTEST_MAIN(TestWallViewer)
#include "tst_wall_viewer.moc"
