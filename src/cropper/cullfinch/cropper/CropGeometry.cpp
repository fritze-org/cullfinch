// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropGeometry.h>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace cullfinch::cropper {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch::cropper::CropGeometry", text);
}

/// How far an edge may sit from the border and still count as on it. Well
/// below a pixel of any preview, well above floating-point noise.
constexpr double kWholeImageTolerance = 1e-4;

bool isEmpty(const QSizeF& image) {
    return !(image.width() > 0.0 && image.height() > 0.0);
}

QRectF toPixelSpace(const QRectF& rect, const QSizeF& image) {
    return {rect.x() * image.width(), rect.y() * image.height(), rect.width() * image.width(),
            rect.height() * image.height()};
}

QPointF toPixelSpace(const QPointF& point, const QSizeF& image) {
    return {std::clamp(point.x(), 0.0, 1.0) * image.width(),
            std::clamp(point.y(), 0.0, 1.0) * image.height()};
}

QRectF toNormalised(const QRectF& rect, const QSizeF& image) {
    return {rect.x() / image.width(), rect.y() / image.height(), rect.width() / image.width(),
            rect.height() / image.height()};
}

/// `rect` moved, never resized beyond the image, until it lies inside it.
QRectF shiftInside(QRectF rect, const QSizeF& image) {
    rect.setWidth(std::min(rect.width(), image.width()));
    rect.setHeight(std::min(rect.height(), image.height()));
    if (rect.left() < 0.0) {
        rect.moveLeft(0.0);
    }
    if (rect.right() > image.width()) {
        rect.moveRight(image.width());
    }
    if (rect.top() < 0.0) {
        rect.moveTop(0.0);
    }
    if (rect.bottom() > image.height()) {
        rect.moveBottom(image.height());
    }
    return rect;
}

/// The minimum side, but never more than a tiny image has.
double minimumSide(const QSizeF& image) {
    return std::min({kMinimumCropSide, image.width(), image.height()});
}

/// Grow `width` x `height` to the minimum and shrink it to the image, keeping
/// `ratio` when there is one.
void limitSize(double& width, double& height, double ratio, const QSizeF& image) {
    const double minimum = minimumSide(image);
    if (ratio > 0.0) {
        width = std::max({width, minimum, minimum * ratio});
        width = std::min({width, image.width(), image.height() * ratio});
        height = width / ratio;
        return;
    }
    width = std::clamp(width, minimum, image.width());
    height = std::clamp(height, minimum, image.height());
}

/// The two edges of a span from a fixed edge to one being dragged, as
/// (start, length), for a length already settled.
std::pair<double, double> span(double fixed, double moving, double length) {
    return {moving >= fixed ? fixed : fixed - length, length};
}

/// Where displayed point (u, v) is found in the stored pixels under EXIF
/// orientation `orientation`. The displayed image is the stored one after the
/// transform the tag names, so this is that transform's inverse.
QPointF storedPoint(const QPointF& point, int orientation) {
    const double u = point.x();
    const double v = point.y();
    switch (orientation) {
    case 2: // Mirrored horizontally.
        return {1.0 - u, v};
    case 3: // Rotated 180 degrees.
        return {1.0 - u, 1.0 - v};
    case 4: // Mirrored vertically.
        return {u, 1.0 - v};
    case 5: // Transposed.
        return {v, u};
    case 6: // Shown rotated 90 degrees clockwise.
        return {v, 1.0 - u};
    case 7: // Transversed.
        return {1.0 - v, 1.0 - u};
    case 8: // Shown rotated 90 degrees counter-clockwise.
        return {1.0 - v, u};
    default:
        return point;
    }
}

} // namespace

std::optional<AspectPreset> presetForDigit(int digit) {
    using enum AspectPreset;
    switch (digit) {
    case 1:
        return Original;
    case 2:
        return ThreeTwo;
    case 3:
        return FourThree;
    case 4:
        return Square;
    case 5:
        return SixteenNine;
    case 6:
        return Free;
    default:
        return std::nullopt;
    }
}

double pixelRatio(AspectChoice choice, const QSizeF& image) {
    if (isEmpty(image)) {
        return 0.0;
    }
    double ratio = 0.0;
    using enum AspectPreset;
    switch (choice.preset) {
    case Original:
        ratio = image.width() / image.height();
        break;
    case ThreeTwo:
        ratio = 3.0 / 2.0;
        break;
    case FourThree:
        ratio = 4.0 / 3.0;
        break;
    case Square:
        ratio = 1.0;
        break;
    case SixteenNine:
        ratio = 16.0 / 9.0;
        break;
    case Free:
        return 0.0;
    }
    // A named shape follows the photo: 3:2 on a portrait photo is 2:3.
    if (choice.preset != Original && image.height() > image.width()) {
        ratio = 1.0 / ratio;
    }
    return choice.swapped ? 1.0 / ratio : ratio;
}

QString aspectLabel(AspectChoice choice, const QSizeF& image) {
    const bool landscape = pixelRatio(choice, image) >= 1.0;
    const auto shape = [landscape](int longSide, int shortSide) {
        return landscape ? QStringLiteral("%1:%2").arg(longSide).arg(shortSide)
                         : QStringLiteral("%1:%2").arg(shortSide).arg(longSide);
    };
    switch (choice.preset) {
    case AspectPreset::Original:
        return choice.swapped ? tr("Original, turned") : tr("Original");
    case AspectPreset::ThreeTwo:
        return shape(3, 2);
    case AspectPreset::FourThree:
        return shape(4, 3);
    case AspectPreset::Square:
        return QStringLiteral("1:1");
    case AspectPreset::SixteenNine:
        return shape(16, 9);
    case AspectPreset::Free:
        return tr("Free");
    }
    return {};
}

bool isWholeImage(const QRectF& rect) {
    return rect.left() <= kWholeImageTolerance && rect.top() <= kWholeImageTolerance &&
           rect.right() >= 1.0 - kWholeImageTolerance &&
           rect.bottom() >= 1.0 - kWholeImageTolerance;
}

QRectF largestAround(const QPointF& centre, double ratio, const QSizeF& image) {
    if (isEmpty(image) || ratio <= 0.0) {
        return wholeImage();
    }
    double width = std::min(image.width(), image.height() * ratio);
    double height = width / ratio;
    const QPointF middle = toPixelSpace(centre, image);
    return toNormalised(
        shiftInside(QRectF(middle.x() - width / 2.0, middle.y() - height / 2.0, width, height),
                    image),
        image);
}

QRectF dragRect(const QPointF& anchor, const QPointF& moving, double ratio, const QSizeF& image) {
    if (isEmpty(image)) {
        return wholeImage();
    }
    const QPointF from = toPixelSpace(anchor, image);
    const QPointF to = toPixelSpace(moving, image);
    const double dx = to.x() - from.x();
    const double dy = to.y() - from.y();

    double width = std::abs(dx);
    double height = std::abs(dy);
    if (ratio > 0.0) {
        // The largest rectangle of the ratio inside the dragged box: the
        // pointer never ends up outside the crop it is drawing.
        if (height <= 0.0 || width / height > ratio) {
            width = height * ratio;
        } else {
            height = width / ratio;
        }
    }
    limitSize(width, height, ratio, image);

    const double left = dx >= 0.0 ? from.x() : from.x() - width;
    const double top = dy >= 0.0 ? from.y() : from.y() - height;
    return toNormalised(shiftInside(QRectF(left, top, width, height), image), image);
}

QRectF adjustRect(const QRectF& start, Handle handle, const QPointF& delta, double ratio,
                  const QSizeF& image) {
    if (isEmpty(image)) {
        return start;
    }
    const QRectF from = toPixelSpace(start, image);
    const QPointF shift(delta.x() * image.width(), delta.y() * image.height());

    using enum Handle;
    switch (handle) {
    case None:
        return start;
    case Move:
        return toNormalised(shiftInside(from.translated(shift), image), image);
    case TopLeft:
        return dragRect(start.bottomRight(), start.topLeft() + delta, ratio, image);
    case TopRight:
        return dragRect(start.bottomLeft(), start.topRight() + delta, ratio, image);
    case BottomLeft:
        return dragRect(start.topRight(), start.bottomLeft() + delta, ratio, image);
    case BottomRight:
        return dragRect(start.topLeft(), start.bottomRight() + delta, ratio, image);
    case Left:
    case Right: {
        const bool left = handle == Left;
        const double fixed = left ? from.right() : from.left();
        const double moving =
            std::clamp((left ? from.left() : from.right()) + shift.x(), 0.0, image.width());
        double width = std::abs(moving - fixed);
        double height = from.height();
        const double middle = from.center().y();
        if (ratio > 0.0) {
            // The other side follows, centred where it was, and stops the
            // drag where it would leave the image.
            const double room = 2.0 * std::min(middle, image.height() - middle);
            width = std::min(width, room * ratio);
        }
        limitSize(width, height, ratio, image);
        const auto [x, w] = span(fixed, moving, width);
        const double y = ratio > 0.0 ? middle - height / 2.0 : from.top();
        return toNormalised(shiftInside(QRectF(x, y, w, height), image), image);
    }
    case Top:
    case Bottom: {
        const bool top = handle == Top;
        const double fixed = top ? from.bottom() : from.top();
        const double moving =
            std::clamp((top ? from.top() : from.bottom()) + shift.y(), 0.0, image.height());
        double height = std::abs(moving - fixed);
        double width = from.width();
        const double middle = from.center().x();
        if (ratio > 0.0) {
            const double room = 2.0 * std::min(middle, image.width() - middle);
            height = std::min(height, room / ratio);
            width = height * ratio;
        }
        limitSize(width, height, ratio, image);
        const auto [y, h] = span(fixed, moving, height);
        const double x = ratio > 0.0 ? middle - width / 2.0 : from.left();
        return toNormalised(shiftInside(QRectF(x, y, width, h), image), image);
    }
    }
    return start;
}

QRect toPixels(const QRectF& rect, const QSize& size) {
    const auto edge = [](double position, int extent) {
        return std::clamp(static_cast<int>(std::lround(position * extent)), 0, extent);
    };
    // At least one pixel, so a degenerate rectangle still names a crop -- even
    // one on the far border, which starts on the last pixel rather than past it.
    const int left = std::min(edge(rect.left(), size.width()), std::max(size.width() - 1, 0));
    const int top = std::min(edge(rect.top(), size.height()), std::max(size.height() - 1, 0));
    const int right = std::max(edge(rect.right(), size.width()), left + 1);
    const int bottom = std::max(edge(rect.bottom(), size.height()), top + 1);
    return {left, top, right - left, bottom - top};
}

QRectF toStoredFrame(const QRectF& rect, int orientation) {
    return QRectF(storedPoint(rect.topLeft(), orientation),
                  storedPoint(rect.bottomRight(), orientation))
        .normalized();
}

} // namespace cullfinch::cropper
