// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

#include <optional>

namespace cullfinch::cropper {

/// The aspect ratios the number keys choose between, in key order.
enum class AspectPreset { Original, ThreeTwo, FourThree, Square, SixteenNine, Free };

/// A preset and whether it is turned on its side.
///
/// A preset names a shape, not an orientation: 3:2 on a portrait photo means
/// 2:3, because that is what somebody cropping a portrait wants nearly every
/// time. `swapped` turns it the other way.
struct AspectChoice {
    AspectPreset preset = AspectPreset::Original;
    bool swapped = false;

    friend bool operator==(const AspectChoice&, const AspectChoice&) = default;
};

/// The preset bound to number key `digit` (1-based), or nothing for a digit
/// no preset is bound to.
[[nodiscard]] std::optional<AspectPreset> presetForDigit(int digit);

/// What the status line shows, such as "3:2", "2:3" or "Original".
[[nodiscard]] QString aspectLabel(AspectChoice choice, const QSizeF& image);

/// Width over height, in image pixels, that `choice` asks for on an image of
/// `image` pixels as it is displayed. Zero means unconstrained.
[[nodiscard]] double pixelRatio(AspectChoice choice, const QSizeF& image);

/// The parts of a crop rectangle a pointer can take hold of.
enum class Handle {
    None,
    Move,
    Left,
    Top,
    Right,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

/// The smallest crop side, in image pixels. Smaller than this is almost
/// certainly a click that slipped rather than a crop anybody wants.
constexpr double kMinimumCropSide = 16.0;

// Every rectangle below is *normalised*: (0,0,1,1) is the whole image as it is
// displayed, after its EXIF orientation. The crop is drawn on a screen-sized
// preview and written from the full-resolution file, and only a normalised
// rectangle means the same thing on both. `image` is the displayed image's
// size in pixels and is what a ratio is measured against.

/// The whole image.
[[nodiscard]] constexpr QRectF wholeImage() {
    return {0.0, 0.0, 1.0, 1.0};
}

/// True when `rect` leaves the image as it is, so writing it would only
/// re-encode the original.
[[nodiscard]] bool isWholeImage(const QRectF& rect);

/// The largest rectangle of `ratio` that fits in the image, as near centred on
/// `centre` as the border allows; the whole image when `ratio` is zero.
///
/// Changing the ratio starts over at the largest crop of the new shape rather
/// than fitting one inside the old rectangle: that would shrink the crop on
/// every change, so swapping orientation twice would not return to where it
/// started. Keeping the centre keeps it near what somebody had drawn.
[[nodiscard]] QRectF largestAround(const QPointF& centre, double ratio, const QSizeF& image);

/// A rectangle drawn from `anchor` towards `moving`, constrained to `ratio`
/// (zero for free), at least `kMinimumCropSide` across and inside the image.
[[nodiscard]] QRectF dragRect(const QPointF& anchor, const QPointF& moving, double ratio,
                              const QSizeF& image);

/// `start` with `handle` dragged by `delta` (normalised), keeping `ratio`.
///
/// A corner moves against the opposite corner. An edge moves against the
/// opposite edge; under a ratio the other side follows, centred where it was.
/// Move translates and stops at the image's border.
[[nodiscard]] QRectF adjustRect(const QRectF& start, Handle handle, const QPointF& delta,
                                double ratio, const QSizeF& image);

/// The pixel rectangle `rect` selects on an image of `size` pixels. Edges are
/// rounded, not truncated, so a crop drawn on a preview and written from the
/// full-resolution file lose at most half a pixel on each side.
[[nodiscard]] QRect toPixels(const QRectF& rect, const QSize& size);

/// `rect`, normalised in the displayed frame, mapped back into the frame the
/// pixels are *stored* in under EXIF orientation `orientation` (1-8; anything
/// else is treated as 1).
///
/// Camera Raw records its crop against the unrotated sensor image, while the
/// crop is drawn on the photo the right way up.
[[nodiscard]] QRectF toStoredFrame(const QRectF& rect, int orientation);

} // namespace cullfinch::cropper
