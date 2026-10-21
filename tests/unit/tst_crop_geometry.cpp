// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropGeometry.h>

#include <QTest>

#include <cmath>

using namespace cullfinch::cropper;

namespace {

/// Close enough for rectangles computed in floating point from pixel sizes.
bool near(const QRectF& actual, const QRectF& expected, double tolerance = 1e-6) {
    return std::abs(actual.left() - expected.left()) <= tolerance &&
           std::abs(actual.top() - expected.top()) <= tolerance &&
           std::abs(actual.right() - expected.right()) <= tolerance &&
           std::abs(actual.bottom() - expected.bottom()) <= tolerance;
}

QByteArray describe(const QRectF& rect) {
    return QStringLiteral("(%1, %2) - (%3, %4)")
        .arg(rect.left())
        .arg(rect.top())
        .arg(rect.right())
        .arg(rect.bottom())
        .toUtf8();
}

/// The ratio of `rect` in pixels on an image of `image`.
double ratioOf(const QRectF& rect, const QSizeF& image) {
    return (rect.width() * image.width()) / (rect.height() * image.height());
}

bool inside(const QRectF& rect) {
    return rect.left() >= 0.0 && rect.top() >= 0.0 && rect.right() <= 1.0 + 1e-9 &&
           rect.bottom() <= 1.0 + 1e-9;
}

const QSizeF kLandscape(600.0, 400.0);
const QSizeF kPortrait(400.0, 600.0);

} // namespace

/// The crop rectangle's arithmetic: shapes, drags, rounding and orientation.
/// No widget and no file; the canvas and the writer only call these.
class TestCropGeometry : public QObject {
    Q_OBJECT

private slots:
    void digitsChooseThePresetsInKeyOrder();
    void aNamedShapeFollowsThePhotosOrientation();
    void theLabelNamesTheShapeAsItIsShown();
    void theLargestCropOfAShapeIsCentredWhereTheOldOneWas();
    void swappingTwiceReturnsToTheStart();
    void drawingFollowsThePointerInEveryDirection();
    void drawingUnderARatioStaysInsideTheDraggedBox();
    void drawingNeverLeavesTheImageOrShrinksBelowTheMinimum();
    void anEdgeMovesAgainstTheOppositeOne();
    void anEdgeUnderARatioKeepsTheOtherSideCentred();
    void aCornerUnderARatioKeepsTheRatio();
    void movingStopsAtTheBorder();
    void pixelEdgesAreRoundedNotTruncated();
    void onlyTheWholeImageCountsAsUncropped();
    void storedFrameUndoesEachOrientation_data();
    void storedFrameUndoesEachOrientation();
};

void TestCropGeometry::digitsChooseThePresetsInKeyOrder() {
    QCOMPARE(presetForDigit(1), std::optional(AspectPreset::Original));
    QCOMPARE(presetForDigit(2), std::optional(AspectPreset::ThreeTwo));
    QCOMPARE(presetForDigit(4), std::optional(AspectPreset::Square));
    QCOMPARE(presetForDigit(6), std::optional(AspectPreset::Free));
    QVERIFY(!presetForDigit(0).has_value());
    QVERIFY(!presetForDigit(7).has_value());
}

void TestCropGeometry::aNamedShapeFollowsThePhotosOrientation() {
    QCOMPARE(pixelRatio({AspectPreset::Original, false}, kLandscape), 1.5);
    QCOMPARE(pixelRatio({AspectPreset::Original, true}, kLandscape), 1.0 / 1.5);
    QCOMPARE(pixelRatio({AspectPreset::ThreeTwo, false}, kPortrait), 2.0 / 3.0);
    QCOMPARE(pixelRatio({AspectPreset::ThreeTwo, true}, kPortrait), 1.5);
    QCOMPARE(pixelRatio({AspectPreset::SixteenNine, false}, kLandscape), 16.0 / 9.0);
    QCOMPARE(pixelRatio({AspectPreset::Square, true}, kLandscape), 1.0);
    QCOMPARE(pixelRatio({AspectPreset::Free, false}, kLandscape), 0.0);
    // Before a preview has arrived there is nothing to measure against.
    QCOMPARE(pixelRatio({AspectPreset::Original, false}, QSizeF()), 0.0);
}

void TestCropGeometry::theLabelNamesTheShapeAsItIsShown() {
    QCOMPARE(aspectLabel({AspectPreset::ThreeTwo, false}, kLandscape), QStringLiteral("3:2"));
    QCOMPARE(aspectLabel({AspectPreset::ThreeTwo, false}, kPortrait), QStringLiteral("2:3"));
    QCOMPARE(aspectLabel({AspectPreset::FourThree, true}, kLandscape), QStringLiteral("3:4"));
    QCOMPARE(aspectLabel({AspectPreset::Square, false}, kLandscape), QStringLiteral("1:1"));
}

void TestCropGeometry::theLargestCropOfAShapeIsCentredWhereTheOldOneWas() {
    // The photo's own shape is the whole photo.
    QVERIFY(near(largestAround({0.5, 0.5}, 1.5, kLandscape), wholeImage()));

    // A square on a 3:2 photo is as tall as the photo, centred.
    const QRectF square = largestAround({0.5, 0.5}, 1.0, kLandscape);
    QVERIFY2(near(square, QRectF(1.0 / 6.0, 0.0, 400.0 / 600.0, 1.0)), describe(square));

    // Centred on a point near the edge, it stops at the border instead.
    const QRectF shifted = largestAround({0.95, 0.5}, 1.0, kLandscape);
    QVERIFY2(near(shifted, QRectF(1.0 - 400.0 / 600.0, 0.0, 400.0 / 600.0, 1.0)),
             describe(shifted));

    // Free has no largest shape: the whole image.
    QVERIFY(near(largestAround({0.2, 0.2}, 0.0, kLandscape), wholeImage()));
}

void TestCropGeometry::swappingTwiceReturnsToTheStart() {
    const QRectF start = wholeImage();
    const QRectF turned = largestAround(start.center(), 1.0 / 1.5, kLandscape);
    QVERIFY(std::abs(ratioOf(turned, kLandscape) - 1.0 / 1.5) < 1e-9);
    const QRectF back = largestAround(turned.center(), 1.5, kLandscape);
    QVERIFY2(near(back, start), describe(back));
}

void TestCropGeometry::drawingFollowsThePointerInEveryDirection() {
    const QSizeF image(1000.0, 1000.0);
    QVERIFY(near(dragRect({0.1, 0.1}, {0.5, 0.6}, 0.0, image), QRectF(0.1, 0.1, 0.4, 0.5)));
    // Up and to the left of the anchor.
    QVERIFY(near(dragRect({0.5, 0.6}, {0.1, 0.1}, 0.0, image), QRectF(0.1, 0.1, 0.4, 0.5)));
    // Up and to the right.
    QVERIFY(near(dragRect({0.1, 0.6}, {0.5, 0.1}, 0.0, image), QRectF(0.1, 0.1, 0.4, 0.5)));
}

void TestCropGeometry::drawingUnderARatioStaysInsideTheDraggedBox() {
    const QSizeF image(1000.0, 500.0);
    // A box twice as wide as tall, under a square ratio: as tall as the box.
    const QRectF square = dragRect({0.0, 0.0}, {1.0, 1.0}, 1.0, image);
    QVERIFY2(near(square, QRectF(0.0, 0.0, 0.5, 1.0)), describe(square));
    // Drawn towards the top left, it grows from the anchor that way.
    const QRectF reversed = dragRect({1.0, 1.0}, {0.0, 0.0}, 1.0, image);
    QVERIFY2(near(reversed, QRectF(0.5, 0.0, 0.5, 1.0)), describe(reversed));
    QCOMPARE(ratioOf(reversed, image), 1.0);
}

void TestCropGeometry::drawingNeverLeavesTheImageOrShrinksBelowTheMinimum() {
    const QRectF beyond = dragRect({0.5, 0.5}, {1.7, -0.4}, 0.0, kLandscape);
    QVERIFY2(inside(beyond), describe(beyond));
    QVERIFY(near(beyond, QRectF(0.5, 0.0, 0.5, 0.5)));

    const QRectF click = dragRect({0.5, 0.5}, {0.5, 0.5}, 0.0, kLandscape);
    QCOMPARE(click.width() * kLandscape.width(), kMinimumCropSide);
    QCOMPARE(click.height() * kLandscape.height(), kMinimumCropSide);

    // At the corner the minimum still fits, by moving inwards.
    const QRectF corner = dragRect({1.0, 1.0}, {1.0, 1.0}, 1.5, kLandscape);
    QVERIFY2(inside(corner), describe(corner));
    QVERIFY(corner.height() * kLandscape.height() >= kMinimumCropSide - 1e-9);
}

void TestCropGeometry::anEdgeMovesAgainstTheOppositeOne() {
    const QRectF start(0.2, 0.2, 0.4, 0.4);
    const QRectF wider = adjustRect(start, Handle::Right, {0.1, 0.3}, 0.0, kLandscape);
    QVERIFY2(near(wider, QRectF(0.2, 0.2, 0.5, 0.4)), describe(wider));
    const QRectF taller = adjustRect(start, Handle::Top, {0.4, -0.1}, 0.0, kLandscape);
    QVERIFY2(near(taller, QRectF(0.2, 0.1, 0.4, 0.5)), describe(taller));
    // Dragged past the opposite edge, it turns over rather than vanishing.
    const QRectF flipped = adjustRect(start, Handle::Left, {0.6, 0.0}, 0.0, kLandscape);
    QVERIFY2(near(flipped, QRectF(0.6, 0.2, 0.2, 0.4)), describe(flipped));
}

void TestCropGeometry::anEdgeUnderARatioKeepsTheOtherSideCentred() {
    const QSizeF image(1000.0, 1000.0);
    const QRectF start(0.4, 0.4, 0.2, 0.2);
    const QRectF wider = adjustRect(start, Handle::Right, {0.2, 0.0}, 1.0, image);
    QVERIFY2(near(wider, QRectF(0.4, 0.3, 0.4, 0.4)), describe(wider));

    // Near the top there is less room above the centre than the drag asks
    // for, so the drag stops where the crop would leave the image.
    const QRectF high(0.4, 0.1, 0.2, 0.2);
    const QRectF capped = adjustRect(high, Handle::Right, {0.6, 0.0}, 1.0, image);
    QVERIFY2(near(capped, QRectF(0.4, 0.0, 0.4, 0.4)), describe(capped));
}

void TestCropGeometry::aCornerUnderARatioKeepsTheRatio() {
    const QRectF start(0.25, 0.25, 0.5, 0.5);
    const double ratio = 1.5;
    const QRectF result = adjustRect(start, Handle::BottomRight, {0.1, 0.05}, ratio, kLandscape);
    QVERIFY2(inside(result), describe(result));
    QVERIFY(std::abs(ratioOf(result, kLandscape) - ratio) < 1e-9);
    // Anchored at the opposite corner.
    QCOMPARE(result.topLeft(), start.topLeft());
}

void TestCropGeometry::movingStopsAtTheBorder() {
    const QRectF start(0.2, 0.2, 0.4, 0.4);
    const QRectF moved = adjustRect(start, Handle::Move, {0.1, -0.1}, 0.0, kLandscape);
    QVERIFY2(near(moved, QRectF(0.3, 0.1, 0.4, 0.4)), describe(moved));
    const QRectF stopped = adjustRect(start, Handle::Move, {0.9, -0.9}, 0.0, kLandscape);
    QVERIFY2(near(stopped, QRectF(0.6, 0.0, 0.4, 0.4)), describe(stopped));
}

void TestCropGeometry::pixelEdgesAreRoundedNotTruncated() {
    QCOMPARE(toPixels(QRectF(0.25, 0.25, 0.5, 0.5), QSize(101, 101)), QRect(25, 25, 51, 51));
    QCOMPARE(toPixels(wholeImage(), QSize(6000, 4000)), QRect(0, 0, 6000, 4000));
    // Even a degenerate rectangle names at least one pixel.
    QCOMPARE(toPixels(QRectF(1.0, 1.0, 0.0, 0.0), QSize(10, 10)).size(), QSize(1, 1));
}

void TestCropGeometry::onlyTheWholeImageCountsAsUncropped() {
    QVERIFY(isWholeImage(wholeImage()));
    QVERIFY(!isWholeImage(QRectF(0.0, 0.0, 1.0, 0.99)));
    QVERIFY(!isWholeImage(QRectF(0.01, 0.0, 0.99, 1.0)));
}

void TestCropGeometry::storedFrameUndoesEachOrientation_data() {
    QTest::addColumn<int>("orientation");
    QTest::addColumn<QRectF>("stored");
    // Where the displayed rectangle (0, 0, 0.5, 0.25) -- the top left, half
    // the width and a quarter of the height -- lies in the stored pixels.
    QTest::newRow("1 normal") << 1 << QRectF(0.0, 0.0, 0.5, 0.25);
    QTest::newRow("2 mirrored") << 2 << QRectF(0.5, 0.0, 0.5, 0.25);
    QTest::newRow("3 upside down") << 3 << QRectF(0.5, 0.75, 0.5, 0.25);
    QTest::newRow("4 flipped") << 4 << QRectF(0.0, 0.75, 0.5, 0.25);
    QTest::newRow("5 transposed") << 5 << QRectF(0.0, 0.0, 0.25, 0.5);
    QTest::newRow("6 clockwise") << 6 << QRectF(0.0, 0.5, 0.25, 0.5);
    QTest::newRow("7 transversed") << 7 << QRectF(0.75, 0.5, 0.25, 0.5);
    QTest::newRow("8 anticlockwise") << 8 << QRectF(0.75, 0.0, 0.25, 0.5);
    QTest::newRow("unknown is normal") << 0 << QRectF(0.0, 0.0, 0.5, 0.25);
}

void TestCropGeometry::storedFrameUndoesEachOrientation() {
    QFETCH(int, orientation);
    QFETCH(QRectF, stored);
    const QRectF mapped = toStoredFrame(QRectF(0.0, 0.0, 0.5, 0.25), orientation);
    QVERIFY2(near(mapped, stored), describe(mapped));
}

QTEST_APPLESS_MAIN(TestCropGeometry)
#include "tst_crop_geometry.moc"
