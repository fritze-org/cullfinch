// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/wall/WallLayout.h>

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

} // namespace

WallLayoutResult WallLayout::compute(const QSizeF& viewport, const QList<LayoutItem>& items,
                                     const WallLayoutOptions& options) {
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

    int bestColumns = 0;
    int bestRows = 0;
    qreal bestCellWidth = 0.0;
    qreal bestCellHeight = 0.0;
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
            bestColumns = columns;
            bestRows = rows;
            bestCellWidth = cellWidth;
            bestCellHeight = cellHeight;
        }
    }

    if (bestColumns == 0) {
        return result;
    }

    // The grid is centred inside the viewport, so a partly filled last row does
    // not push the block off to one side.
    const qreal gridWidth = (bestCellWidth * bestColumns) + (options.spacing * (bestColumns - 1));
    const qreal gridHeight = (bestCellHeight * bestRows) + (options.spacing * (bestRows - 1));
    const qreal originX = (viewport.width() - gridWidth) / 2.0;
    const qreal originY = (viewport.height() - gridHeight) / 2.0;

    result.columns = bestColumns;
    result.rows = bestRows;
    result.smallestImageArea = bestScore;
    result.cells.reserve(count);

    // Selection order is preserved, filling row by row.
    for (int index = 0; index < count; ++index) {
        const int row = index / bestColumns;
        const int column = index % bestColumns;

        LayoutCell cell;
        cell.id = items.at(index).id;
        cell.cellRect =
            QRectF(originX + (static_cast<qreal>(column) * (bestCellWidth + options.spacing)),
                   originY + (static_cast<qreal>(row) * (bestCellHeight + options.spacing)),
                   bestCellWidth, bestCellHeight);

        const QSizeF size = effectiveSize(items.at(index), options);
        const qreal scale = std::min(bestCellWidth / size.width(), bestCellHeight / size.height());
        const QSizeF fitted(size.width() * scale, size.height() * scale);
        cell.imageRect = QRectF(cell.cellRect.x() + ((bestCellWidth - fitted.width()) / 2.0),
                                cell.cellRect.y() + ((bestCellHeight - fitted.height()) / 2.0),
                                fitted.width(), fitted.height());

        result.cells.append(cell);
    }

    return result;
}

} // namespace cullfinch::flows::wall
