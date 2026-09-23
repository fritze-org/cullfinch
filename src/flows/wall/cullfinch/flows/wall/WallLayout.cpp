// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/wall/WallLayout.h>

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cullfinch::flows::wall {
namespace {

QSizeF effectiveSize(const LayoutItem& item, const WallLayoutOptions& options) {
    if (item.imageSize.width() > 0.0 && item.imageSize.height() > 0.0) {
        return item.imageSize;
    }
    return QSizeF(options.placeholderAspect, 1.0);
}

/// Area of `size` scaled to fit entirely inside a cell, preserving aspect.
qreal fittedArea(const QSizeF& size, qreal cellWidth, qreal cellHeight) {
    const qreal scale = std::min(cellWidth / size.width(), cellHeight / size.height());
    const qreal width = size.width() * scale;
    const qreal height = size.height() * scale;
    return width * height;
}

/// The grid shape a layout settled on, before any cell is placed.
struct Grid {
    int columns = 0;
    int rows = 0;
    qreal cellWidth = 0.0;
    qreal cellHeight = 0.0;
    QPointF origin;
};

/// Place every item into `grid`, row by row in selection order, each image
/// fitted inside its cell and never cropped.
void placeCells(const QList<LayoutItem>& items, const Grid& grid, const WallLayoutOptions& options,
                WallLayoutResult& result) {
    const auto count = static_cast<int>(items.size());
    result.columns = grid.columns;
    result.rows = grid.rows;
    result.cells.reserve(count);

    qreal smallest = std::numeric_limits<qreal>::max();
    for (int index = 0; index < count; ++index) {
        const int row = index / grid.columns;
        const int column = index % grid.columns;

        LayoutCell cell;
        cell.id = items.at(index).id;
        cell.cellRect = QRectF(
            grid.origin.x() + (static_cast<qreal>(column) * (grid.cellWidth + options.spacing)),
            grid.origin.y() + (static_cast<qreal>(row) * (grid.cellHeight + options.spacing)),
            grid.cellWidth, grid.cellHeight);

        const QSizeF size = effectiveSize(items.at(index), options);
        const qreal scale =
            std::min(grid.cellWidth / size.width(), grid.cellHeight / size.height());
        const QSizeF fitted(size.width() * scale, size.height() * scale);
        cell.imageRect = QRectF(cell.cellRect.x() + ((grid.cellWidth - fitted.width()) / 2.0),
                                cell.cellRect.y() + ((grid.cellHeight - fitted.height()) / 2.0),
                                fitted.width(), fitted.height());
        smallest = std::min(smallest, fitted.width() * fitted.height());

        result.cells.append(cell);
    }
    result.smallestImageArea = smallest;
}

/// A grid of cells about `options.cellWidth` wide that fills the viewport's
/// width and grows downwards as far as it needs to.
///
/// The cells stretch so that a row fills the width exactly rather than leaving
/// a ragged gap at the right, and a cell wider than the viewport is narrowed to
/// fit it: nothing ever scrolls sideways. The grid starts at the top, because a
/// scrolled grid centred vertically would move every tile whenever the last
/// row changed.
WallLayoutResult computeFixedWidth(const QSizeF& viewport, const QList<LayoutItem>& items,
                                   const WallLayoutOptions& options) {
    WallLayoutResult result;
    const auto count = static_cast<int>(items.size());
    const qreal availableWidth = viewport.width() - (2.0 * options.margin);
    if (count == 0 || availableWidth <= 0.0) {
        return result;
    }

    Grid grid;
    grid.columns = std::max(1, static_cast<int>(std::floor((availableWidth + options.spacing) /
                                                           (options.cellWidth + options.spacing))));
    grid.rows = (count + grid.columns - 1) / grid.columns;
    grid.cellWidth = (availableWidth - (options.spacing * (grid.columns - 1))) /
                     static_cast<qreal>(grid.columns);
    if (grid.cellWidth <= 0.0) {
        return result;
    }
    grid.cellHeight = grid.cellWidth / options.placeholderAspect;
    grid.origin = QPointF(options.margin, options.margin);

    placeCells(items, grid, options, result);
    result.contentSize =
        QSizeF(viewport.width(), (2.0 * options.margin) + (grid.cellHeight * grid.rows) +
                                     (options.spacing * (grid.rows - 1)));
    return result;
}

} // namespace

WallLayoutResult WallLayout::compute(const QSizeF& viewport, const QList<LayoutItem>& items,
                                     const WallLayoutOptions& options) {
    if (options.cellWidth > 0.0) {
        return computeFixedWidth(viewport, items, options);
    }

    WallLayoutResult result;
    const auto count = static_cast<int>(items.size());
    if (count == 0 || viewport.width() <= 0.0 || viewport.height() <= 0.0) {
        return result;
    }

    const qreal availableWidth = viewport.width() - (2.0 * options.margin);
    const qreal availableHeight = viewport.height() - (2.0 * options.margin);
    if (availableWidth <= 0.0 || availableHeight <= 0.0) {
        return result;
    }

    Grid best;
    qreal bestScore = 0.0;

    for (int columns = 1; columns <= count; ++columns) {
        const int rows = (count + columns - 1) / columns;

        const qreal cellWidth =
            (availableWidth - (options.spacing * (columns - 1))) / static_cast<qreal>(columns);
        const qreal cellHeight =
            (availableHeight - (options.spacing * (rows - 1))) / static_cast<qreal>(rows);
        if (cellWidth <= 0.0 || cellHeight <= 0.0) {
            continue;
        }

        qreal smallest = std::numeric_limits<qreal>::max();
        for (const LayoutItem& item : items) {
            smallest =
                std::min(smallest, fittedArea(effectiveSize(item, options), cellWidth, cellHeight));
        }

        // Strictly greater keeps the smallest column count on a tie.
        if (smallest > bestScore) {
            bestScore = smallest;
            best.columns = columns;
            best.rows = rows;
            best.cellWidth = cellWidth;
            best.cellHeight = cellHeight;
        }
    }

    if (best.columns == 0) {
        return result;
    }

    // The grid is centred inside the viewport, so a partly filled last row does
    // not push the block off to one side.
    const qreal gridWidth =
        (best.cellWidth * best.columns) + (options.spacing * (best.columns - 1));
    const qreal gridHeight = (best.cellHeight * best.rows) + (options.spacing * (best.rows - 1));
    best.origin =
        QPointF((viewport.width() - gridWidth) / 2.0, (viewport.height() - gridHeight) / 2.0);

    placeCells(items, best, options, result);
    result.contentSize = viewport;
    return result;
}

} // namespace cullfinch::flows::wall
