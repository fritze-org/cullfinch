// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/FakeImageService.h>
#include <cullfinch/ui/PreviewLoader.h>

#include <QImage>
#include <QSignalSpy>
#include <QTest>

using namespace cullfinch;

namespace {

ui::AssetPresentation presentationFor(const QString& member, const QString& path) {
    ui::AssetPresentation presentation;
    presentation.id = domain::AssetId(member + QStringLiteral("-asset"));
    presentation.previewMemberId = domain::MemberId(member);
    presentation.displayName = path;
    presentation.previewPath = QStringLiteral("/photos/") + path;
    presentation.previewFingerprint.sizeBytes = 1024;
    return presentation;
}

} // namespace

/// The decode side of a canvas, without a canvas.
///
/// Readiness is load-bearing: hosts keep decision input disabled until every
/// required preview has arrived, so "ready" arriving early, late or for the
/// wrong photo is a correctness bug in the comparison flows.
class TestPreviewLoader : public QObject {
    Q_OBJECT

private slots:
    void requestsCarryTheSourceAndTheAskedForSize();
    void readinessFollowsTheFittedPreview();
    void aResultForAnotherPhotoIsIgnored();
    void aResultFromASupersededGenerationIsIgnored();
    void aDecodeFailureReportsAnErrorInsteadOfReadiness();
    void aSucceedingRetryClearsTheError();
    void retryingWithoutAnErrorDoesNothing();
    void fullResolutionIsSeparateFromReadiness();
    void aNewSourceDropsEverythingTheOldOneHad();
};

void TestPreviewLoader::requestsCarryTheSourceAndTheAskedForSize() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);

    QVERIFY(!loader.hasSource());
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 7);
    QVERIFY(loader.hasSource());
    QCOMPARE(loader.generation(), quint64{7});

    loader.requestFitted(QSize(800, 600));
    loader.requestFullResolution();

    QCOMPARE(images.requests().size(), 2);
    QCOMPARE(images.requests().at(0).kind, application::ImageRequestClass::Comparison);
    QCOMPARE(images.requests().at(0).targetSize, QSize(800, 600));
    QCOMPARE(images.requests().at(0).path, QStringLiteral("/photos/IMG_1.JPG"));
    QCOMPARE(images.requests().at(0).generation, quint64{7});
    QCOMPARE(images.requests().at(1).kind, application::ImageRequestClass::FullResolution);
    // Native resolution is asked for with an unset size, never with a guess:
    // the decoder scales only for a size that is valid and not empty.
    QVERIFY(!images.requests().at(1).targetSize.isValid());
    // The fitted preview outranks the refinement nobody may ever zoom into.
    QVERIFY(images.requests().at(0).priority > images.requests().at(1).priority);
}

void TestPreviewLoader::readinessFollowsTheFittedPreview() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    QSignalSpy readiness(&loader, &ui::PreviewLoader::readinessChanged);
    QSignalSpy changed(&loader, &ui::PreviewLoader::changed);

    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    QCOMPARE(readiness.size(), 1);
    QCOMPARE(readiness.takeFirst().at(0).toBool(), false);
    QVERIFY(!loader.isReady());

    loader.requestFitted(QSize(800, 600));
    QVERIFY(!loader.isReady()); // Asking is not arriving.

    changed.clear();
    images.succeed(0);
    QCOMPARE(readiness.size(), 1);
    QCOMPARE(readiness.takeFirst().at(0).toBool(), true);
    QCOMPARE(changed.size(), 1);
    QVERIFY(loader.isReady());
    QVERIFY(!loader.fitted().isNull());
    QCOMPARE(loader.nativeSize(), QSize(400, 300));
}

void TestPreviewLoader::aResultForAnotherPhotoIsIgnored() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    loader.requestFitted(QSize(800, 600));

    application::ImageResult stray = images.answerFor(0);
    stray.memberId = domain::MemberId(QStringLiteral("m2"));
    stray.success = true;
    stray.image = QImage(4, 4, QImage::Format_RGB32);
    QSignalSpy readiness(&loader, &ui::PreviewLoader::readinessChanged);
    images.deliver(stray);

    // Another canvas's image must never be reported as this photo's, or a
    // decision would be made against pixels from a different file.
    QVERIFY(!loader.isReady());
    QVERIFY(loader.fitted().isNull());
    QCOMPARE(readiness.size(), 0);
}

void TestPreviewLoader::aResultFromASupersededGenerationIsIgnored() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    const ui::AssetPresentation photo =
        presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG"));
    loader.setSource(photo, 1);
    loader.requestFitted(QSize(800, 600));

    // The same photo, re-presented after the collection was rescanned.
    loader.setSource(photo, 2);
    QSignalSpy readiness(&loader, &ui::PreviewLoader::readinessChanged);
    images.succeed(0); // The answer to the request from generation 1.

    QVERIFY(!loader.isReady());
    QCOMPARE(readiness.size(), 0);
}

void TestPreviewLoader::aDecodeFailureReportsAnErrorInsteadOfReadiness() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    loader.requestFitted(QSize(800, 600));

    QSignalSpy readiness(&loader, &ui::PreviewLoader::readinessChanged);
    images.fail(0, QStringLiteral("not a JPEG"));

    // A decode error is a reported failure, never a rejection decision: the
    // photo stays undecidable rather than becoming eliminable.
    QVERIFY(loader.hasError());
    QCOMPARE(loader.errorText(), QStringLiteral("not a JPEG"));
    QVERIFY(!loader.isReady());
    QCOMPARE(readiness.size(), 1);
    QCOMPARE(readiness.takeFirst().at(0).toBool(), false);
}

void TestPreviewLoader::aSucceedingRetryClearsTheError() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    loader.requestFitted(QSize(800, 600));
    images.fail(0, QStringLiteral("not a JPEG"));

    QSignalSpy retried(&loader, &ui::PreviewLoader::retryRequested);
    QSignalSpy changed(&loader, &ui::PreviewLoader::changed);
    QVERIFY(loader.retryFitted(QSize(800, 600)));

    QCOMPARE(retried.size(), 1);
    // The message goes the moment it stops being true, so nothing keeps an
    // error on screen that the loader no longer reports.
    QVERIFY(!loader.hasError());
    QCOMPARE(changed.size(), 1);
    QCOMPARE(images.requests().size(), 2);

    images.succeed(1);
    QVERIFY(loader.isReady());
    QVERIFY(!loader.hasError());
}

void TestPreviewLoader::retryingWithoutAnErrorDoesNothing() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    loader.requestFitted(QSize(800, 600));
    images.succeed(0);

    // Nothing failed, so the key that offered the retry stays unhandled and a
    // working preview is not thrown away and decoded again.
    QVERIFY(!loader.retryFitted(QSize(800, 600)));
    QCOMPARE(images.requests().size(), 1);
}

void TestPreviewLoader::fullResolutionIsSeparateFromReadiness() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    loader.requestFitted(QSize(800, 600));
    loader.requestFullResolution();

    QSignalSpy readiness(&loader, &ui::PreviewLoader::readinessChanged);
    images.succeed(1, QSize(1200, 800)); // The native-resolution answer first.

    // Inspection has what it needs; a decision still does not, because the
    // fitted preview is what the canvas draws.
    QVERIFY(loader.isFullResolutionReady());
    QVERIFY(!loader.full().isNull());
    QVERIFY(!loader.isReady());
    QCOMPARE(readiness.size(), 0);

    images.succeed(0);
    QVERIFY(loader.isReady());
    QCOMPARE(readiness.size(), 1);
}

void TestPreviewLoader::aNewSourceDropsEverythingTheOldOneHad() {
    testsupport::FakeImageService images;
    ui::PreviewLoader loader(images);
    loader.setSource(presentationFor(QStringLiteral("m1"), QStringLiteral("IMG_1.JPG")), 1);
    loader.requestFitted(QSize(800, 600));
    loader.requestFullResolution();
    images.succeed(0);
    images.succeed(1);
    QVERIFY(loader.isReady());
    QVERIFY(loader.isFullResolutionReady());

    QSignalSpy readiness(&loader, &ui::PreviewLoader::readinessChanged);
    loader.setSource(presentationFor(QStringLiteral("m2"), QStringLiteral("IMG_2.JPG")), 1);

    // The next photo starts from nothing. Keeping the previous pixels would put
    // a photo on screen that the host believes it has replaced.
    QCOMPARE(readiness.size(), 1);
    QCOMPARE(readiness.takeFirst().at(0).toBool(), false);
    QVERIFY(!loader.isReady());
    QVERIFY(!loader.isFullResolutionReady());
    QVERIFY(loader.fitted().isNull());
    QVERIFY(loader.full().isNull());
    QVERIFY(!loader.hasError());
    QVERIFY(loader.nativeSize().isEmpty());
}

QTEST_MAIN(TestPreviewLoader)
#include "tst_preview_loader.moc"
