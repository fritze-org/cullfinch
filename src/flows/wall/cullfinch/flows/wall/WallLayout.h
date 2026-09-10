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
};

struct WallLayoutResult {
    QList<LayoutCell> cells;
    int columns = 0;
    int rows = 0;
    /// The area of the smallest fitted image; the quantity the search
    /// maximises. Zero when nothing could be placed.
    qreal smallestImageArea = 0.0;
};

/// Deterministic equal-cell grid.
///
/// For each candidate column count the corresponding row count and cell size
/// are computed, and the layout that maximises the smallest fitted-image area
/// wins. Ties break towards the smaller column count, so the result is stable
/// across runs and platforms.
///
/// Every candidate is placed: the wall never silently paginates or omits one.
/// When the selection is large the tiles simply become smaller.
class WallLayout {
public:
    [[nodiscard]] static WallLayoutResult compute(const QSizeF& viewport,
                                                  const QList<LayoutItem>& items,
                                                  const WallLayoutOptions& options = {});
};

} // namespace cullfinch::flows::wall
