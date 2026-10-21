// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuiFixture.h"

#include <cullfinch/cropper/CropGeometry.h>
#include <cullfinch/cropper/CropWindow.h>
#include <cullfinch/testsupport/FakeImageService.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <memory>

using namespace cullfinch;
using namespace cullfinch::cropper;

namespace {

/// The size every preview is answered with: a 3:2 landscape photo.
constexpr QSize kPreviewSize(300, 200);
constexpr QSize kWindowSize(900, 700);

/// What the window asked to have written. Filled on the writer's thread.
struct SaveLog {
    QMutex mutex;
    QList<CropJob> jobs;
    QString failWith;

    [[nodiscard]] QList<CropJob> taken() {
        const QMutexLocker locker(&mutex);
        return jobs;
    }
};

CropPlan planOf(int count) {
    CropPlan plan;
    for (int index = 1; index <= count; ++index) {
        const QString name = QStringLiteral("IMG_%1").arg(index);
        CropTarget target;
        target.presentation.id = domain::AssetId(name);
        target.presentation.previewMemberId = domain::MemberId(name + QStringLiteral("-jpg"));
        target.presentation.displayName = name;
        target.presentation.previewPath =
            QStringLiteral("/photos/") + name + QStringLiteral(".jpg");
        target.jpegPath = target.presentation.previewPath;
        plan.targets.append(target);
    }
    return plan;
}

/// Events go straight to the widget: a headless compositor can have no input
/// seat, so synthesising them through the platform would test the session.
void sendKey(QWidget* widget, Qt::Key key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
}

void sendMouse(QWidget* widget, QEvent::Type type, const QPointF& at, Qt::MouseButtons buttons) {
    QMouseEvent event(type, at, widget->mapToGlobal(at), Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &event);
}

void drag(QWidget* widget, const QPointF& from, const QPointF& to) {
    sendMouse(widget, QEvent::MouseButtonPress, from, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, (from + to) / 2.0, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, to, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, to, Qt::NoButton);
}

bool near(const QRectF& actual, const QRectF& expected, double tolerance = 0.01) {
    return std::abs(actual.left() - expected.left()) <= tolerance &&
           std::abs(actual.top() - expected.top()) <= tolerance &&
           std::abs(actual.right() - expected.right()) <= tolerance &&
           std::abs(actual.bottom() - expected.bottom()) <= tolerance;
}

double pixelRatioOf(const QRectF& rect) {
    return (rect.width() * kPreviewSize.width()) / (rect.height() * kPreviewSize.height());
}

} // namespace

/// The crop tool's window against an image service that answers on command
/// and a writer that only records: stepping, saving, skipping and going back.
class TestCropTool : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void nothingHappensBeforeThePhotoArrives();
    void enterOnAnUntouchedPhotoWritesNothing();
    void aDrawnCropIsWrittenAndTheNextPhotoShown();
    void goingBackShowsTheCropAcceptedThere();
    void aClickWithoutADragKeepsTheCrop();
    void aDragInsideACropMovesIt();
    void theNumberKeysAndRChooseTheShape();
    void escapeStartsTheCropOver();
    void passingTheLastPhotoClosesTheWindow();
    void aFailedWriteKeepsTheWindowOpen();
    void aWriteThatFailsWhileClosingKeepsTheWindowOpen();

private:
    void show(int count);
    /// Answer every request made so far, so the photo on screen is ready.
    void deliverEverything();
    /// A point on the photo, given as a fraction of it.
    [[nodiscard]] QPointF onPhoto(double x, double y) const;

    std::unique_ptr<testsupport::FakeImageService> images_;
    std::shared_ptr<SaveLog> log_;
    std::unique_ptr<CropWindow> window_;
    qsizetype answered_ = 0;
};

void TestCropTool::initTestCase() {
    guitests::requirePlatform(qEnvironmentVariable("CULLFINCH_EXPECTED_PLATFORM"));
}

void TestCropTool::cleanup() {
    window_.reset(); // Before the service its loaders hold a reference to.
    images_.reset();
    log_.reset();
    answered_ = 0;
}

void TestCropTool::show(int count) {
    images_ = std::make_unique<testsupport::FakeImageService>();
    log_ = std::make_shared<SaveLog>();
    window_ =
        std::make_unique<CropWindow>(*images_, planOf(count), [log = log_](const CropJob& job) {
            const QMutexLocker locker(&log->mutex);
            log->jobs.append(job);
            return log->failWith;
        });
    window_->show();
    QVERIFY(guitests::settleWindowSize(window_.get(), kWindowSize));
}

void TestCropTool::deliverEverything() {
    // Resizes keep asking for a preview at the new size; answer them all,
    // including the next photo the window decodes ahead.
    for (int round = 0; round < 3; ++round) {
        for (; answered_ < images_->requests().size(); ++answered_) {
            images_->succeed(answered_, kPreviewSize);
        }
        QCoreApplication::processEvents();
    }
}

QPointF TestCropTool::onPhoto(double x, double y) const {
    const QRectF photo = window_->canvas()->imageRect();
    return {photo.x() + x * photo.width(), photo.y() + y * photo.height()};
}

void TestCropTool::nothingHappensBeforeThePhotoArrives() {
    show(2);
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(!window_->canvas()->isReady());
    sendKey(window_.get(), Qt::Key_Return);
    QCOMPARE(window_->currentIndex(), 0);
    window_->waitForSaves();
    QVERIFY(log_->taken().isEmpty());
}

void TestCropTool::enterOnAnUntouchedPhotoWritesNothing() {
    show(2);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    QVERIFY(window_->canvas()->isReady());
    // The default shape is the photo's own, so the crop starts as all of it.
    QVERIFY(isWholeImage(window_->canvas()->cropRect()));

    sendKey(window_.get(), Qt::Key_Return);
    QCOMPARE(window_->currentIndex(), 1);
    window_->waitForSaves();
    QVERIFY(log_->taken().isEmpty());
}

void TestCropTool::aDrawnCropIsWrittenAndTheNextPhotoShown() {
    show(3);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();

    // A box half as wide and half as tall as the photo is 3:2 itself, so the
    // default shape leaves it as drawn.
    drag(window_->canvas(), onPhoto(0.25, 0.25), onPhoto(0.75, 0.75));
    const QRectF drawn = window_->canvas()->cropRect();
    QVERIFY2(near(drawn, QRectF(0.25, 0.25, 0.5, 0.5)), qPrintable(QStringLiteral("%1 %2 %3 %4")
                                                                       .arg(drawn.x())
                                                                       .arg(drawn.y())
                                                                       .arg(drawn.width())
                                                                       .arg(drawn.height())));

    sendKey(window_.get(), Qt::Key_Return);
    QCOMPARE(window_->currentIndex(), 1);
    window_->waitForSaves();

    const QList<CropJob> jobs = log_->taken();
    QCOMPARE(jobs.size(), 1);
    QCOMPARE(jobs.first().jpegPath, QStringLiteral("/photos/IMG_1.jpg"));
    QCOMPARE(jobs.first().rect, drawn);
    QCOMPARE(window_->pendingSaves(), 0);
}

void TestCropTool::goingBackShowsTheCropAcceptedThere() {
    show(2);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    drag(window_->canvas(), onPhoto(0.1, 0.1), onPhoto(0.4, 0.4));
    const QRectF accepted = window_->canvas()->cropRect();
    sendKey(window_.get(), Qt::Key_Return);
    window_->waitForSaves();
    deliverEverything();
    QVERIFY(isWholeImage(window_->canvas()->cropRect()));

    sendKey(window_.get(), Qt::Key_Left);
    QCOMPARE(window_->currentIndex(), 0);
    deliverEverything();
    QCOMPARE(window_->canvas()->cropRect(), accepted);

    // A skipped photo has nothing to remember.
    sendKey(window_.get(), Qt::Key_Space);
    deliverEverything();
    QCOMPARE(window_->currentIndex(), 1);
    QVERIFY(isWholeImage(window_->canvas()->cropRect()));
}

void TestCropTool::aClickWithoutADragKeepsTheCrop() {
    show(1);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    window_->canvas()->setCropRect(QRectF(0.4, 0.4, 0.2, 0.2));
    const QPointF outside = onPhoto(0.1, 0.1);
    sendMouse(window_->canvas(), QEvent::MouseButtonPress, outside, Qt::LeftButton);
    sendMouse(window_->canvas(), QEvent::MouseButtonRelease, outside, Qt::NoButton);
    QCOMPARE(window_->canvas()->cropRect(), QRectF(0.4, 0.4, 0.2, 0.2));
}

void TestCropTool::aDragInsideACropMovesIt() {
    show(1);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    window_->canvas()->setCropRect(QRectF(0.4, 0.4, 0.2, 0.2));
    drag(window_->canvas(), onPhoto(0.5, 0.5), onPhoto(0.6, 0.4));
    QVERIFY(near(window_->canvas()->cropRect(), QRectF(0.5, 0.3, 0.2, 0.2)));
}

void TestCropTool::theNumberKeysAndRChooseTheShape() {
    show(2);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    auto* aspect = window_->findChild<QLabel*>(QStringLiteral("cropAspect"));
    QVERIFY(aspect != nullptr);
    QCOMPARE(aspect->text(), QStringLiteral("Original"));

    sendKey(window_.get(), Qt::Key_4);
    QCOMPARE(aspect->text(), QStringLiteral("1:1"));
    QVERIFY(std::abs(pixelRatioOf(window_->canvas()->cropRect()) - 1.0) < 0.01);

    sendKey(window_.get(), Qt::Key_2);
    sendKey(window_.get(), Qt::Key_R);
    QCOMPARE(aspect->text(), QStringLiteral("2:3"));
    QVERIFY(std::abs(pixelRatioOf(window_->canvas()->cropRect()) - 2.0 / 3.0) < 0.01);

    // The shape carries over to the next photo.
    sendKey(window_.get(), Qt::Key_Space);
    deliverEverything();
    QCOMPARE(aspect->text(), QStringLiteral("2:3"));
    QVERIFY(std::abs(pixelRatioOf(window_->canvas()->cropRect()) - 2.0 / 3.0) < 0.01);
}

void TestCropTool::escapeStartsTheCropOver() {
    show(1);
    if (QTest::currentTestFailed()) {
        return;
    }
    deliverEverything();
    drag(window_->canvas(), onPhoto(0.1, 0.1), onPhoto(0.3, 0.3));
    QVERIFY(!isWholeImage(window_->canvas()->cropRect()));
    sendKey(window_.get(), Qt::Key_Escape);
    QVERIFY(isWholeImage(window_->canvas()->cropRect()));
}

void TestCropTool::passingTheLastPhotoClosesTheWindow() {
    show(2);
    if (QTest::currentTestFailed()) {
        return;
    }
    QSignalSpy finished(window_.get(), &CropWindow::sequenceFinished);
    deliverEverything();
    sendKey(window_.get(), Qt::Key_Space);
    deliverEverything();
    drag(window_->canvas(), onPhoto(0.2, 0.2), onPhoto(0.6, 0.6));
    sendKey(window_.get(), Qt::Key_Return);

    // Closes once the last write has landed, not before.
    QVERIFY(finished.wait(5000));
    QVERIFY(!window_->isVisible());
    QCOMPARE(log_->taken().size(), 1);
}

void TestCropTool::aFailedWriteKeepsTheWindowOpen() {
    show(1);
    if (QTest::currentTestFailed()) {
        return;
    }
    log_->failWith = QStringLiteral("disk full");
    QSignalSpy saved(window_.get(), &CropWindow::saveFinished);
    QSignalSpy finished(window_.get(), &CropWindow::sequenceFinished);
    deliverEverything();
    drag(window_->canvas(), onPhoto(0.2, 0.2), onPhoto(0.6, 0.6));
    sendKey(window_.get(), Qt::Key_Return);

    QVERIFY(saved.wait(5000));
    QCOMPARE(finished.count(), 0);
    QVERIFY(window_->isVisible());
    QCOMPARE(window_->failures(), QStringList{QStringLiteral("disk full")});
    auto* message = window_->findChild<QLabel*>(QStringLiteral("cropMessage"));
    QVERIFY(message != nullptr);
    QVERIFY2(message->text().contains(QStringLiteral("disk full")), qPrintable(message->text()));

    // Back on the photo, the crop that was never written is not shown as if it
    // had been.
    sendKey(window_.get(), Qt::Key_Left);
    deliverEverything();
    QVERIFY(isWholeImage(window_->canvas()->cropRect()));
}

void TestCropTool::aWriteThatFailsWhileClosingKeepsTheWindowOpen() {
    show(2);
    if (QTest::currentTestFailed()) {
        return;
    }
    log_->failWith = QStringLiteral("disk full");
    deliverEverything();
    drag(window_->canvas(), onPhoto(0.2, 0.2), onPhoto(0.6, 0.6));
    sendKey(window_.get(), Qt::Key_Return);

    // Closed while the write is still outstanding: waiting for it is what
    // brings the failure in, and the window must not go before it is seen.
    QVERIFY(!window_->close());
    QVERIFY(window_->isVisible());
    QCOMPARE(window_->failures(), QStringList{QStringLiteral("disk full")});

    // Nothing is pending any more, so the next attempt to leave goes through.
    QVERIFY(window_->close());
}

QTEST_MAIN(TestCropTool)
#include "tst_crop_tool.moc"
