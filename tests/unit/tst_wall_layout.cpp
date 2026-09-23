// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/wall/WallLayout.h>

#include <QTest>

using namespace cullfinch::flows::wall;
using cullfinch::domain::AssetId;

namespace {

QList<LayoutItem> items(int count, const QSizeF& size) {
    QList<LayoutItem> result;
    for (int index = 0; index < count; ++index) {
        result.append(LayoutItem{AssetId(QStringLiteral("a%1").arg(index)), size});
    }
    return result;
}

} // namespace

class TestWallLayout : public QObject {
    Q_OBJECT

private slots:
    void placesEveryCandidate_data();
    void placesEveryCandidate();
    void cellsStayInsideTheViewport();
    void cellsDoNotOverlap();
    void imagesAreNeverCropped();
    void preservesSelectionOrder();
    void handlesMixedPortraitAndLandscape();
    void isDeterministic();
    void degradesGracefullyOnATinyViewport();
    void fitsTheViewportUnlessACellWidthIsAsked();
    void aCellWidthFillsTheRowAndGrowsDownwards_data();
    void aCellWidthFillsTheRowAndGrowsDownwards();
    void aCellWiderThanTheViewportIsNarrowedToFit();
    void aCellWidthKeepsImagesWholeAndInOrder();
};

void TestWallLayout::placesEveryCandidate_data() {
    QTest::addColumn<int>("count");
    for (int count : {1, 2, 3, 5, 12, 37, 100, 250}) {
        QTest::newRow(qPrintable(QStringLiteral("n=%1").arg(count))) << count;
    }
}

void TestWallLayout::placesEveryCandidate() {
    QFETCH(int, count);
    const WallLayoutResult layout =
        WallLayout::compute(QSizeF(1600, 900), items(count, QSizeF(3000, 2000)));

    // Smaller tiles, never a dropped or hidden candidate.
    QCOMPARE(static_cast<int>(layout.cells.size()), count);
    QVERIFY(layout.columns >= 1);
    QVERIFY(layout.rows * layout.columns >= count);
    QVERIFY(layout.smallestImageArea > 0.0);
}

void TestWallLayout::cellsStayInsideTheViewport() {
    const QSizeF viewport(1280, 800);
    const WallLayoutResult layout = WallLayout::compute(viewport, items(23, QSizeF(4000, 3000)));

    const QRectF bounds(0, 0, viewport.width(), viewport.height());
    for (const LayoutCell& cell : layout.cells) {
        QVERIFY2(bounds.contains(cell.cellRect), "a cell escaped the viewport");
        QVERIFY2(cell.cellRect.contains(cell.imageRect), "an image escaped its cell");
    }
}

void TestWallLayout::cellsDoNotOverlap() {
    const WallLayoutResult layout =
        WallLayout::compute(QSizeF(1000, 700), items(17, QSizeF(1600, 1200)));

    for (int i = 0; i < layout.cells.size(); ++i) {
        for (int j = i + 1; j < layout.cells.size(); ++j) {
            const QRectF intersection =
                layout.cells.at(i).cellRect.intersected(layout.cells.at(j).cellRect);
            QVERIFY2(intersection.isEmpty(), "two cells overlap");
        }
    }
}

void TestWallLayout::imagesAreNeverCropped() {
    const QSizeF source(4032, 3024);
    const WallLayoutResult layout = WallLayout::compute(QSizeF(1400, 900), items(9, source));

    const qreal sourceAspect = source.width() / source.height();
    for (const LayoutCell& cell : layout.cells) {
        QVERIFY(cell.imageRect.width() > 0.0);
        QVERIFY(cell.imageRect.height() > 0.0);
        // The whole image is fitted, so the aspect ratio is preserved exactly.
        const qreal aspect = cell.imageRect.width() / cell.imageRect.height();
        QVERIFY2(qAbs(aspect - sourceAspect) < 0.001, "the fitted image changed shape");
        QVERIFY(cell.imageRect.width() <= cell.cellRect.width() + 0.001);
        QVERIFY(cell.imageRect.height() <= cell.cellRect.height() + 0.001);
    }
}

void TestWallLayout::preservesSelectionOrder() {
    const QList<LayoutItem> input = items(11, QSizeF(1600, 1200));
    const WallLayoutResult layout = WallLayout::compute(QSizeF(1200, 800), input);

    for (int index = 0; index < input.size(); ++index) {
        QCOMPARE(layout.cells.at(index).id, input.at(index).id);
    }
}

void TestWallLayout::handlesMixedPortraitAndLandscape() {
    QList<LayoutItem> mixed;
    for (int index = 0; index < 10; ++index) {
        mixed.append(LayoutItem{AssetId(QStringLiteral("a%1").arg(index)),
                                (index % 2 == 0) ? QSizeF(4000, 3000) : QSizeF(3000, 4000)});
    }

    const WallLayoutResult layout = WallLayout::compute(QSizeF(1400, 1000), mixed);
    QCOMPARE(layout.cells.size(), mixed.size());
    for (int index = 0; index < mixed.size(); ++index) {
        const LayoutCell& cell = layout.cells.at(index);
        const qreal expected =
            mixed.at(index).imageSize.width() / mixed.at(index).imageSize.height();
        const qreal actual = cell.imageRect.width() / cell.imageRect.height();
        QVERIFY2(qAbs(actual - expected) < 0.001, "a mixed-orientation image was cropped");
    }
}

void TestWallLayout::isDeterministic() {
    const QList<LayoutItem> input = items(13, QSizeF(3000, 2000));
    const WallLayoutResult first = WallLayout::compute(QSizeF(1024, 768), input);
    const WallLayoutResult second = WallLayout::compute(QSizeF(1024, 768), input);

    QCOMPARE(first.columns, second.columns);
    QCOMPARE(first.rows, second.rows);
    for (int index = 0; index < first.cells.size(); ++index) {
        QCOMPARE(first.cells.at(index).cellRect, second.cells.at(index).cellRect);
    }
}

void TestWallLayout::degradesGracefullyOnATinyViewport() {
    // Not enough room for anything: an empty layout, never a crash or a
    // negative-size cell.
    const WallLayoutResult layout = WallLayout::compute(QSizeF(4, 4), items(20, QSizeF(100, 100)));
    for (const LayoutCell& cell : layout.cells) {
        QVERIFY(cell.cellRect.width() > 0.0);
        QVERIFY(cell.cellRect.height() > 0.0);
    }
    QVERIFY(WallLayout::compute(QSizeF(0, 0), items(3, QSizeF(10, 10))).cells.isEmpty());
    QVERIFY(WallLayout::compute(QSizeF(100, 100), {}).cells.isEmpty());
}

void TestWallLayout::fitsTheViewportUnlessACellWidthIsAsked() {
    // The comparison wall never sets a cell width, and must lay out exactly as
    // it always has: everything inside the viewport, however small.
    const QSizeF viewport(1200, 800);
    const WallLayoutResult fitted = WallLayout::compute(viewport, items(60, QSizeF(3000, 2000)));
    QCOMPARE(fitted.contentSize, viewport);
    const QRectF bounds(QPointF(0, 0), viewport);
    for (const LayoutCell& cell : fitted.cells) {
        QVERIFY(bounds.contains(cell.cellRect));
    }

    WallLayoutOptions options;
    options.cellWidth = 240.0;
    const WallLayoutResult fixed =
        WallLayout::compute(viewport, items(60, QSizeF(3000, 2000)), options);
    QCOMPARE(fixed.cells.size(), 60);
    QVERIFY2(fixed.contentSize.height() > viewport.height(),
             "sixty 240-pixel tiles cannot fit in 800 pixels; the grid has to grow instead");
    QCOMPARE(fixed.contentSize.width(), viewport.width());
}

void TestWallLayout::aCellWidthFillsTheRowAndGrowsDownwards_data() {
    QTest::addColumn<qreal>("viewportWidth");
    QTest::addColumn<qreal>("cellWidth");
    QTest::addColumn<int>("count");
    QTest::addColumn<int>("columns");

    // (width - 2 * margin + spacing) / (cell + spacing), rounded down.
    QTest::newRow("exact") << qreal(1016) << qreal(240) << 10 << 4;
    QTest::newRow("slack stretches the cells") << qreal(1100) << qreal(240) << 10 << 4;
    QTest::newRow("fewer photos than columns") << qreal(1016) << qreal(240) << 3 << 4;
    QTest::newRow("small tiles") << qreal(1016) << qreal(96) << 100 << 9;
}

void TestWallLayout::aCellWidthFillsTheRowAndGrowsDownwards() {
    QFETCH(qreal, viewportWidth);
    QFETCH(qreal, cellWidth);
    QFETCH(int, count);
    QFETCH(int, columns);

    WallLayoutOptions options;
    options.cellWidth = cellWidth;
    // The height is irrelevant once a cell width is set; zero proves it.
    const WallLayoutResult layout =
        WallLayout::compute(QSizeF(viewportWidth, 0), items(count, QSizeF(3000, 2000)), options);

    QCOMPARE(layout.columns, columns);
    QCOMPARE(layout.rows, (count + columns - 1) / columns);
    QCOMPARE(static_cast<int>(layout.cells.size()), count);

    const LayoutCell& first = layout.cells.constFirst();
    QVERIFY2(first.cellRect.width() >= cellWidth - 0.001, "a cell came out narrower than asked");
    QCOMPARE(first.cellRect.topLeft(), QPointF(options.margin, options.margin));

    // A full row reaches the right margin exactly, with no ragged gap.
    const qreal rowRight =
        first.cellRect.x() + (columns * first.cellRect.width()) + ((columns - 1) * options.spacing);
    QVERIFY(qAbs(rowRight - (viewportWidth - options.margin)) < 0.001);

    // Every cell is inside the content area the caller is told to scroll over.
    const QRectF content(QPointF(0, 0), layout.contentSize);
    for (const LayoutCell& cell : layout.cells) {
        QVERIFY(content.contains(cell.cellRect));
    }
    const LayoutCell& last = layout.cells.constLast();
    QVERIFY(qAbs(layout.contentSize.height() - (last.cellRect.bottom() + options.margin)) < 0.001);
}

void TestWallLayout::aCellWiderThanTheViewportIsNarrowedToFit() {
    WallLayoutOptions options;
    options.cellWidth = 2000.0;
    const WallLayoutResult layout =
        WallLayout::compute(QSizeF(600, 400), items(3, QSizeF(3000, 2000)), options);

    // One column, never a grid that has to scroll sideways.
    QCOMPARE(layout.columns, 1);
    QCOMPARE(layout.rows, 3);
    for (const LayoutCell& cell : layout.cells) {
        QVERIFY(cell.cellRect.right() <= 600.0 - options.margin + 0.001);
    }

    QVERIFY(WallLayout::compute(QSizeF(4, 400), items(3, QSizeF(10, 10)), options).cells.isEmpty());
    QVERIFY(WallLayout::compute(QSizeF(600, 400), {}, options).cells.isEmpty());
}

void TestWallLayout::aCellWidthKeepsImagesWholeAndInOrder() {
    QList<LayoutItem> mixed;
    for (int index = 0; index < 9; ++index) {
        mixed.append(LayoutItem{AssetId(QStringLiteral("a%1").arg(index)),
                                (index % 3 == 0) ? QSizeF(3000, 4000) : QSizeF(4000, 3000)});
    }

    WallLayoutOptions options;
    options.cellWidth = 180.0;
    const WallLayoutResult first = WallLayout::compute(QSizeF(900, 300), mixed, options);
    const WallLayoutResult second = WallLayout::compute(QSizeF(900, 300), mixed, options);

    QCOMPARE(first.cells.size(), mixed.size());
    for (int index = 0; index < mixed.size(); ++index) {
        const LayoutCell& cell = first.cells.at(index);
        QCOMPARE(cell.id, mixed.at(index).id);
        QCOMPARE(cell.cellRect, second.cells.at(index).cellRect);
        QVERIFY(cell.cellRect.contains(cell.imageRect));
        const qreal expected =
            mixed.at(index).imageSize.width() / mixed.at(index).imageSize.height();
        QVERIFY(qAbs((cell.imageRect.width() / cell.imageRect.height()) - expected) < 0.001);
    }
    QVERIFY(first.smallestImageArea > 0.0);
}

QTEST_APPLESS_MAIN(TestWallLayout)
#include "tst_wall_layout.moc"
