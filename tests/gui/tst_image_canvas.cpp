// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/FakeImageService.h>
#include <cullfinch/ui/ImageCanvas.h>

#include <QCoreApplication>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>

#include <memory>
#include <tuple>

using namespace cullfinch;

namespace {

ui::AssetPresentation photoNamed(const QString& name) {
    ui::AssetPresentation presentation;
    presentation.id = domain::AssetId(name);
    presentation.previewMemberId = domain::MemberId(name + QStringLiteral("-jpg"));
    presentation.displayName = name;
    presentation.previewPath = QStringLiteral("/photos/") + name;
    presentation.pairingText = QStringLiteral("JPG only");
    return presentation;
}

/// Events go straight to the widget. A headless compositor can have no input
/// seat, so synthesising them through the platform would test the session
/// rather than the canvas.
void sendMouse(QWidget* widget, QEvent::Type type, Qt::MouseButton button, const QPoint& at) {
    QMouseEvent event(type, QPointF(at), QPointF(widget->mapToGlobal(at)), button,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : button, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &event);
}

void sendMove(QWidget* widget, const QPoint& to) {
    QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(widget->mapToGlobal(to)), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &move);
}

void sendKey(QWidget* widget, Qt::Key key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
}

void sendWheel(QWidget* widget, int degrees) {
    const QPointF centre(widget->rect().center());
    QWheelEvent wheel(centre, QPointF(widget->mapToGlobal(widget->rect().center())), QPoint(0, 0),
                      QPoint(0, degrees), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(widget, &wheel);
}

/// The pixel size the canvas is expected to ask for, read back from the widget
/// rather than assumed: a window manager may have given it another size.
QSize expectedTargetOf(const ui::ImageCanvas& canvas) {
    const qreal ratio = canvas.devicePixelRatioF();
    return {static_cast<int>(canvas.width() * ratio), static_cast<int>(canvas.height() * ratio)};
}

} // namespace

/// The canvas on its own, against an image service that answers only when this
/// test says so.
///
/// The view suites drive the canvas through a flow and a real decoder, which
/// cannot say when a preview arrives or make one fail on demand. Those are the
/// orderings that decide whether a photo can be eliminated, so they are pinned
/// down here instead.
class TestImageCanvas : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void presentingAPhotoAsksForAScreenSizedPreview();
    void aResizeAsksForThePixelsTheNewSizeNeeds();
    void decisionInputWaitsForThePreview();
    void aDecodeErrorIsReportedAndRetryingAsksAgain();
    void inspectionLoadsFullResolutionAndPanningDecidesNothing();
    void anInterruptedGestureDecidesNothing();
    void otherButtonsAndKeysAreLeftToTheWidget();
    void clearingThePresentationLeavesNothingToDecideOn();
    void theMarksAndTheCaptionAreDrawn();

private:
    /// A canvas showing a photo whose fitted preview has arrived.
    void presentAndDeliver();
    /// Answers whatever the canvas asked for last. Naming the request by
    /// index would be wrong the moment a repaint or a resize asks again.
    void deliverLatest(const QSize& size = QSize(400, 300));
    void failLatest(const QString& error);

    /// A fresh service per test: what a canvas asked for is only readable as
    /// a count if an earlier case's requests are not still in the list.
    std::unique_ptr<testsupport::FakeImageService> images_;
    /// The canvas lives in a window rather than being one: a window manager
    /// may answer a top-level resize with a size of its own, while a child's
    /// geometry is the application's to decide.
    std::unique_ptr<QWidget> window_;
    ui::ImageCanvas* canvas_ = nullptr;
};

void TestImageCanvas::initTestCase() {
    // The GUI suites state which backend they ran on, so an accidental
    // offscreen fallback fails the desktop-backend job instead of passing
    // quietly. Spelled out here rather than taken from the shared fixture,
    // which owns a whole composition root this suite has no use for.
    if (const QString expected = qEnvironmentVariable("CULLFINCH_EXPECTED_PLATFORM");
        !expected.isEmpty()) {
        QCOMPARE(QGuiApplication::platformName(), expected);
    }
}

void TestImageCanvas::init() {
    images_ = std::make_unique<testsupport::FakeImageService>();
    window_ = std::make_unique<QWidget>();
    window_->resize(420, 340);
    canvas_ = new ui::ImageCanvas(*images_, window_.get());
    canvas_->setGeometry(10, 10, 320, 240);
    window_->show();
    QCoreApplication::processEvents();
}

void TestImageCanvas::cleanup() {
    canvas_ = nullptr; // Owned by the window, and going with it.
    window_.reset();   // Before the service it holds a reference to.
    images_.reset();
}

void TestImageCanvas::deliverLatest(const QSize& size) {
    QVERIFY(!images_->requests().isEmpty());
    images_->succeed(images_->requests().size() - 1, size);
}

void TestImageCanvas::failLatest(const QString& error) {
    QVERIFY(!images_->requests().isEmpty());
    images_->fail(images_->requests().size() - 1, error);
}

void TestImageCanvas::presentAndDeliver() {
    canvas_->setPresentation(photoNamed(QStringLiteral("IMG_1.JPG")), 1);
    deliverLatest();
    QVERIFY(canvas_->preview()->isReady());
}

void TestImageCanvas::presentingAPhotoAsksForAScreenSizedPreview() {
    // Nothing is decoded for a canvas that has not been given a photo.
    QCOMPARE(images_->requests().size(), 0);

    QSignalSpy readiness(canvas_->preview(), &ui::PreviewLoader::readinessChanged);
    canvas_->setPresentation(photoNamed(QStringLiteral("IMG_1.JPG")), 4);

    QVERIFY(!images_->requests().isEmpty());
    const application::ImageRequest& asked = images_->requests().constLast();
    QCOMPARE(asked.kind, application::ImageRequestClass::Comparison);
    QCOMPARE(asked.memberId, domain::MemberId(QStringLiteral("IMG_1.JPG-jpg")));
    QCOMPARE(asked.generation, quint64{4});
    QCOMPARE(asked.targetSize, expectedTargetOf(*canvas_));

    // A photo that has not arrived yet is not one anybody can judge.
    QCOMPARE(readiness.size(), 1);
    QCOMPARE(readiness.takeFirst().at(0).toBool(), false);
    QVERIFY(!canvas_->preview()->isReady());
    QVERIFY(canvas_->imageRect().isEmpty());
    std::ignore = canvas_->grab(); // The "Loading…" state paints.

    deliverLatest();
    QVERIFY(canvas_->preview()->isReady());
    QVERIFY(!canvas_->imageRect().isEmpty());
    std::ignore = canvas_->grab();
}

void TestImageCanvas::aResizeAsksForThePixelsTheNewSizeNeeds() {
    presentAndDeliver();
    const int before = images_->requests().size();

    canvas_->setGeometry(canvas_->x(), canvas_->y(), canvas_->width() * 2, canvas_->height() * 2);
    QCoreApplication::processEvents();

    // A larger widget needs more pixels than the preview it already has.
    QVERIFY(images_->requests().size() > before);
    QCOMPARE(images_->requests().constLast().kind, application::ImageRequestClass::Comparison);
    QCOMPARE(images_->requests().constLast().targetSize, expectedTargetOf(*canvas_));
}

void TestImageCanvas::decisionInputWaitsForThePreview() {
    int eliminations = 0;
    connect(canvas_, &ui::ImageCanvas::eliminateRequested, this,
            [&eliminations](ui::ImageCanvas::ActivationSource) { ++eliminations; });
    QSignalSpy armed(canvas_, &ui::ImageCanvas::gestureArmed);

    canvas_->setPresentation(photoNamed(QStringLiteral("IMG_1.JPG")), 1);
    const QPoint centre = canvas_->rect().center();
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);

    // The gesture is reported either way -- a host binds it to what is under
    // the pointer -- but it decides nothing until the photo is visible.
    QCOMPARE(armed.size(), 1);
    QCOMPARE(eliminations, 0);

    // Nor does the keyboard.
    sendKey(canvas_, Qt::Key_Delete);
    QCOMPARE(eliminations, 0);

    deliverLatest();
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);
    QCOMPARE(eliminations, 1);

    sendKey(canvas_, Qt::Key_Backspace);
    QCOMPARE(eliminations, 2);
}

void TestImageCanvas::aDecodeErrorIsReportedAndRetryingAsksAgain() {
    int eliminations = 0;
    connect(canvas_, &ui::ImageCanvas::eliminateRequested, this,
            [&eliminations](ui::ImageCanvas::ActivationSource) { ++eliminations; });

    canvas_->setPresentation(photoNamed(QStringLiteral("IMG_1.JPG")), 1);
    QSignalSpy retried(canvas_->preview(), &ui::PreviewLoader::retryRequested);
    failLatest(QStringLiteral("not a JPEG"));

    // A decode error is a reported failure, never a rejection decision.
    QVERIFY(canvas_->preview()->hasError());
    QCOMPARE(canvas_->accessibleDescription(), QStringLiteral("not a JPEG"));
    std::ignore = canvas_->grab(); // The error and its retry offer paint.
    const QPoint centre = canvas_->rect().center();
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);
    QCOMPARE(eliminations, 0);

    const int beforeRetry = images_->requests().size();
    sendKey(canvas_, Qt::Key_R);
    QCOMPARE(retried.size(), 1);
    QCOMPARE(images_->requests().size(), beforeRetry + 1);
    QCOMPARE(images_->requests().constLast().kind, application::ImageRequestClass::Comparison);
    QVERIFY(!canvas_->preview()->hasError());

    deliverLatest();
    QVERIFY(canvas_->preview()->isReady());

    // With nothing failing, R is not the canvas's key to take.
    const int afterRetry = images_->requests().size();
    sendKey(canvas_, Qt::Key_R);
    QCOMPARE(images_->requests().size(), afterRetry);
    QCOMPARE(retried.size(), 1);
}

void TestImageCanvas::inspectionLoadsFullResolutionAndPanningDecidesNothing() {
    presentAndDeliver();
    int eliminations = 0;
    connect(canvas_, &ui::ImageCanvas::eliminateRequested, this,
            [&eliminations](ui::ImageCanvas::ActivationSource) { ++eliminations; });
    QSignalSpy inspected(canvas_, &ui::ImageCanvas::inspectToggled);
    QSignalSpy viewChanged(canvas_, &ui::ImageCanvas::viewChanged);

    // Asking for the mode it is already in is not a toggle.
    canvas_->setInspecting(false);
    QVERIFY(!canvas_->isInspecting());
    QCOMPARE(inspected.size(), 0);

    sendKey(canvas_, Qt::Key_Space);
    QVERIFY(canvas_->isInspecting());
    QCOMPARE(inspected.size(), 1);
    QCOMPARE(images_->requests().constLast().kind, application::ImageRequestClass::FullResolution);
    std::ignore = canvas_->grab(); // "loading full resolution" paints.

    deliverLatest(QSize(1200, 800));
    QVERIFY(canvas_->preview()->isFullResolutionReady());
    std::ignore = canvas_->grab(); // The 100% window paints.

    const QPoint from = canvas_->rect().center();
    const QPoint to = from + QPoint(40, 25);
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, from);
    sendMove(canvas_, to);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, to);

    // Panning moved the view, and a pan is not an elimination.
    QVERIFY(!viewChanged.isEmpty());
    QVERIFY(canvas_->normalisedCentre() != QPointF(0.5, 0.5));
    QCOMPARE(eliminations, 0);

    sendWheel(canvas_, 120);
    QVERIFY(canvas_->zoom() > 1.0);

    // Leaving inspection puts the framing back where a fitted photo starts.
    sendKey(canvas_, Qt::Key_Space);
    QVERIFY(!canvas_->isInspecting());
    QCOMPARE(canvas_->normalisedCentre(), QPointF(0.5, 0.5));
    QCOMPARE(canvas_->zoom(), 1.0);
}

void TestImageCanvas::anInterruptedGestureDecidesNothing() {
    presentAndDeliver();
    int eliminations = 0;
    connect(canvas_, &ui::ImageCanvas::eliminateRequested, this,
            [&eliminations](ui::ImageCanvas::ActivationSource) { ++eliminations; });

    // Released outside: the gesture no longer refers to this photo.
    const QPoint centre = canvas_->rect().center();
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton,
              QPoint(canvas_->width() + 20, canvas_->height() + 20));
    QCOMPARE(eliminations, 0);

    // The desktop took the focus away mid-gesture.
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    QFocusEvent lost(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
    QCoreApplication::sendEvent(canvas_, &lost);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);
    QCOMPARE(eliminations, 0);

    // A fresh, uninterrupted gesture still decides.
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);
    QCOMPARE(eliminations, 1);

    // The window stopped being the active one mid-gesture: a workspace switch,
    // a dialog, the screen locking. Hidden here, because a test cannot ask a
    // compositor to take the activation away.
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    window_->hide();
    // Deactivation comes back from the compositor, so it is waited for rather
    // than assumed to have happened by the time hide() returns.
    QVERIFY(QTest::qWaitFor([this]() { return !canvas_->isActiveWindow(); }, 5000));
    QEvent deactivated(QEvent::ActivationChange);
    QCoreApplication::sendEvent(canvas_, &deactivated);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);
    QCOMPARE(eliminations, 1);
}

void TestImageCanvas::otherButtonsAndKeysAreLeftToTheWidget() {
    presentAndDeliver();
    int eliminations = 0;
    connect(canvas_, &ui::ImageCanvas::eliminateRequested, this,
            [&eliminations](ui::ImageCanvas::ActivationSource) { ++eliminations; });
    QSignalSpy armed(canvas_, &ui::ImageCanvas::gestureArmed);
    QSignalSpy viewChanged(canvas_, &ui::ImageCanvas::viewChanged);

    const QPoint centre = canvas_->rect().center();
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::RightButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::RightButton, centre);
    QCOMPARE(armed.size(), 0);
    QCOMPARE(eliminations, 0);

    sendKey(canvas_, Qt::Key_A);
    QCOMPARE(eliminations, 0);

    // The wheel zooms while inspecting and belongs to whatever scrolls
    // otherwise.
    sendWheel(canvas_, 120);
    QCOMPARE(viewChanged.size(), 0);
    QCOMPARE(canvas_->zoom(), 1.0);

    // Holding a key must not reject a sequence of photos.
    QKeyEvent autoRepeat(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier, 0, 0, 0, QString(),
                         true);
    QCoreApplication::sendEvent(canvas_, &autoRepeat);
    QCOMPARE(eliminations, 0);
}

void TestImageCanvas::clearingThePresentationLeavesNothingToDecideOn() {
    presentAndDeliver();
    const int before = images_->requests().size();

    canvas_->clearPresentation();

    // There is no file to decode, so nothing is asked for and nothing is ready.
    QCOMPARE(images_->requests().size(), before);
    QVERIFY(!canvas_->preview()->isReady());
    QVERIFY(!canvas_->preview()->hasSource());
    QVERIFY(!canvas_->presentation().id.isValid());
    QCOMPARE(canvas_->accessibleName(), QStringLiteral("No photo"));
    std::ignore = canvas_->grab(); // The empty pane paints.

    int eliminations = 0;
    connect(canvas_, &ui::ImageCanvas::eliminateRequested, this,
            [&eliminations](ui::ImageCanvas::ActivationSource) { ++eliminations; });
    const QPoint centre = canvas_->rect().center();
    sendMouse(canvas_, QEvent::MouseButtonPress, Qt::LeftButton, centre);
    sendMouse(canvas_, QEvent::MouseButtonRelease, Qt::LeftButton, centre);
    QCOMPARE(eliminations, 0);
}

void TestImageCanvas::theMarksAndTheCaptionAreDrawn() {
    presentAndDeliver();

    canvas_->setRejected(true);
    canvas_->setSelectionHighlighted(true);
    canvas_->setCaption(QStringLiteral("IMG_1.JPG · JPG + 1 RAW"));
    QVERIFY(canvas_->isRejected());
    QVERIFY(canvas_->isSelectionHighlighted());
    // Not colour alone: the eliminated mark is in the accessible name too.
    QCOMPARE(canvas_->accessibleName(), QStringLiteral("IMG_1.JPG, eliminated"));
    std::ignore = canvas_->grab();

    // Hosts re-apply the marks on every state update, which must not redraw or
    // re-announce anything.
    canvas_->setRejected(true);
    canvas_->setSelectionHighlighted(true);
    QCOMPARE(canvas_->accessibleName(), QStringLiteral("IMG_1.JPG, eliminated"));

    // A different photo arrives unmarked.
    canvas_->setPresentation(photoNamed(QStringLiteral("IMG_2.JPG")), 1);
    QVERIFY(!canvas_->isRejected());
    QVERIFY(!canvas_->isSelectionHighlighted());
    QCOMPARE(canvas_->accessibleName(), QStringLiteral("IMG_2.JPG"));
}

QTEST_MAIN(TestImageCanvas)
#include "tst_image_canvas.moc"
