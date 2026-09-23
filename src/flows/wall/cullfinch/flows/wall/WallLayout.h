// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/Ids.h>

#include <QList>
#include <QRectF>
#include <QSizeF>

namespace cullfinch::flows::wall {

using domain::AssetId;

/// One candidate to place, with the size of its *oriented* image.
struct LayoutItem {
    AssetId id;
    QSizeF imageSize;
};

/// Where a candidate ended up. `cellRect` is its share of the grid;
/// `imageRect` is the complete image fitted inside it, never cropped.
struct LayoutCell {
    AssetId id;
    QRectF cellRect;
    QRectF imageRect;
};

struct WallLayoutOptions {
    qreal margin = 8.0;
    qreal spacing = 8.0;
    /// Aspect ratio assumed while an image's real size is still unknown, so
    /// tiles do not jump once decoding finishes.
    qreal placeholderAspect = 3.0 / 2.0;
    /// When positive, cells are about this wide and the grid grows downwards
    /// past the viewport, for a scroll area to show, instead of every tile
    /// shrinking until all of them fit. Only the viewport's width is used then.
    /// Zero keeps the fit-to-viewport search the comparison wall relies on.
    qreal cellWidth = 0.0;
};

struct WallLayoutResult {
    QList<LayoutCell> cells;
    int columns = 0;
    int rows = 0;
    /// The area of the smallest fitted image; the quantity the search
    /// maximises. Zero when nothing could be placed.
    qreal smallestImageArea = 0.0;
    /// The area the grid needs, margins included. The viewport itself when the
    /// grid is fitted into it; taller than the viewport when `cellWidth` asked
    /// for tiles that do not all fit.
    QSizeF contentSize{0.0, 0.0};
};

/// Deterministic equal-cell grid.
///
/// For each candidate column count the corresponding row count and cell size
/// are computed, and the layout that maximises the smallest fitted-image area
/// wins. Ties break towards the smaller column count, so the result is stable
/// across runs and platforms.
///
/// Every candidate is placed: the wall never silently paginates or omits one.
/// When the selection is large the tiles simply become smaller -- unless the
/// caller fixed a cell width, in which case the grid becomes taller instead.
class WallLayout {
public:
    [[nodiscard]] static WallLayoutResult compute(const QSizeF& viewport,
                                                  const QList<LayoutItem>& items,
                                                  const WallLayoutOptions& options = {});
};

} // namespace cullfinch::flows::wall
