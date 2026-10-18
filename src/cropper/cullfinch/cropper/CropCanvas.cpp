// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropCanvas.h>

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>

#include <array>
#include <cmath>

namespace cullfinch::cropper {

namespace {

/// Room around the photo, so a handle on its border is not cut off.
constexpr int kMargin = 16;
/// How far from an edge or corner a press still takes hold of it.
constexpr qreal kGrabDistance = 10.0;
constexpr qreal kHandleSize = 8.0;
/// A press outside the crop has to move this far before it draws a new one,
/// so a click that slipped off the rectangle does not throw it away.
constexpr int kDrawThreshold = 4;
/// A resize is followed by the next until it settles; the preview on screen is
/// scaled meanwhile rather than decoded again at every intermediate size.
constexpr int kRefinementDelayMs = 150;

const QColor kBackground(24, 24, 24);
const QColor kShade(0, 0, 0, 160);
const QColor kGuide(255, 255, 255, 90);

} // namespace

CropCanvas::CropCanvas(application::IImageService& images, QWidget* parent)
    : QWidget(parent), preview_(new ui::PreviewLoader(images, this)),
      refinement_(new QTimer(this)) {
    setObjectName(QStringLiteral("cropCanvas"));
    // Keys belong to the window, which also has to see them while the pointer
    // is over the photo.
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setMinimumSize(320, 240);

    refinement_->setSingleShot(true);
    refinement_->setInterval(kRefinementDelayMs);
    connect(refinement_, &QTimer::timeout, this, &CropCanvas::requestPreview);
    connect(preview_, &ui::PreviewLoader::readinessChanged, this, &CropCanvas::onReadinessChanged);
    connect(preview_, &ui::PreviewLoader::changed, this, qOverload<>(&QWidget::update));
}

void CropCanvas::setPhoto(const ui::AssetPresentation& presentation, quint64 generation,
                          const std::optional<QRectF>& remembered) {
    drag_ = {};
    refinement_->stop();
    preview_->setSource(presentation, generation);
    crop_ = wholeImage();
    pendingCrop_ = remembered;
    awaitingFirstPreview_ = true;
    requestPreview();
    updateCursor(Handle::None);
    update();
}

void CropCanvas::setCropRect(const QRectF& rect) {
    const QRectF clamped = rect.normalized().intersected(wholeImage());
    changeCrop(clamped.isEmpty() ? wholeImage() : clamped);
}

void CropCanvas::resetCrop() {
    changeCrop(largestAround({0.5, 0.5}, pixelRatio(aspect_, imageSize()), imageSize()));
}

void CropCanvas::setAspect(AspectChoice choice) {
    aspect_ = choice;
    if (!isReady() || choice.preset == AspectPreset::Free) {
        // Nothing to measure against yet, or nothing to constrain: Free keeps
        // whatever was drawn under the previous shape.
        update();
        return;
    }
    changeCrop(largestAround(crop_.center(), pixelRatio(aspect_, imageSize()), imageSize()));
}

QSizeF CropCanvas::imageSize() const {
    return preview_->fitted().isNull() ? QSizeF() : QSizeF(preview_->fitted().size());
}

QRectF CropCanvas::imageRect() const {
    const QSizeF source = imageSize();
    const QRectF area = QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
    if (source.isEmpty() || area.isEmpty()) {
        return {};
    }
    const QSizeF fitted = source.scaled(area.size(), Qt::KeepAspectRatio);
    return {area.x() + (area.width() - fitted.width()) / 2.0,
            area.y() + (area.height() - fitted.height()) / 2.0, fitted.width(), fitted.height()};
}

QRectF CropCanvas::cropWidgetRect() const {
    const QRectF image = imageRect();
    return {image.x() + crop_.x() * image.width(), image.y() + crop_.y() * image.height(),
            crop_.width() * image.width(), crop_.height() * image.height()};
}

QSize CropCanvas::targetPixels() const {
    const qreal ratio = devicePixelRatioF();
    return {static_cast<int>(width() * ratio), static_cast<int>(height() * ratio)};
}

void CropCanvas::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.fillRect(rect(), kBackground);

    const QImage& image = preview_->fitted();
    if (image.isNull()) {
        painter.setPen(Qt::lightGray);
        const QString text = preview_->hasError() ? preview_->errorText() : tr("Loading…");
        painter.drawText(rect().adjusted(kMargin, kMargin, -kMargin, -kMargin),
                         Qt::AlignCenter | Qt::TextWordWrap, text);
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF photo = imageRect();
    painter.drawImage(photo, image);

    // Everything the crop leaves out is shaded rather than hidden, so the
    // decision about where to cut can still see what it is cutting off.
    const QRectF crop = cropWidgetRect();
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(photo);
    outside.addRect(crop);
    painter.fillPath(outside, kShade);

    // Thirds, the guide most crops are composed against.
    painter.setPen(QPen(kGuide, 1.0));
    for (const qreal third : {1.0 / 3.0, 2.0 / 3.0}) {
        const qreal x = crop.left() + crop.width() * third;
        const qreal y = crop.top() + crop.height() * third;
        painter.drawLine(QPointF(x, crop.top()), QPointF(x, crop.bottom()));
        painter.drawLine(QPointF(crop.left(), y), QPointF(crop.right(), y));
    }

    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawRect(crop);
    const std::array<QPointF, 8> handles{crop.topLeft(),
                                         crop.topRight(),
                                         crop.bottomLeft(),
                                         crop.bottomRight(),
                                         QPointF(crop.center().x(), crop.top()),
                                         QPointF(crop.center().x(), crop.bottom()),
                                         QPointF(crop.left(), crop.center().y()),
                                         QPointF(crop.right(), crop.center().y())};
    for (const QPointF& centre : handles) {
        painter.fillRect(QRectF(centre.x() - kHandleSize / 2.0, centre.y() - kHandleSize / 2.0,
                                kHandleSize, kHandleSize),
                         Qt::white);
    }
}

void CropCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (preview_->fitted().isNull()) {
        requestPreview(); // Nothing on screen to scale in the meantime.
        return;
    }
    refinement_->start();
}

void CropCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !isReady()) {
        QWidget::mousePressEvent(event);
        return;
    }
    drag_.active = true;
    drag_.handle = handleAt(event->position());
    drag_.pressPosition = event->position().toPoint();
    drag_.origin = toNormalised(event->position());
    drag_.startRect = crop_;
    event->accept();
}

void CropCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!drag_.active) {
        updateCursor(handleAt(event->position()));
        return;
    }
    const QSizeF image = imageSize();
    const double ratio = pixelRatio(aspect_, image);
    const QPointF here = toNormalised(event->position());
    if (drag_.handle == Handle::None) {
        if ((event->position().toPoint() - drag_.pressPosition).manhattanLength() <
            kDrawThreshold) {
            return;
        }
        changeCrop(dragRect(drag_.origin, here, ratio, image));
    } else {
        changeCrop(adjustRect(drag_.startRect, drag_.handle, here - drag_.origin, ratio, image));
    }
    event->accept();
}

void CropCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    drag_ = {};
    updateCursor(handleAt(event->position()));
    event->accept();
}

Handle CropCanvas::handleAt(const QPointF& position) const {
    if (!isReady()) {
        return Handle::None;
    }
    const QRectF crop = cropWidgetRect();
    const auto near = [](qreal a, qreal b) { return std::abs(a - b) <= kGrabDistance; };
    const auto within = [](qreal value, qreal low, qreal high) {
        return value >= low - kGrabDistance && value <= high + kGrabDistance;
    };
    const bool left = near(position.x(), crop.left());
    const bool right = near(position.x(), crop.right());
    const bool top = near(position.y(), crop.top());
    const bool bottom = near(position.y(), crop.bottom());
    const bool alongX = within(position.x(), crop.left(), crop.right());
    const bool alongY = within(position.y(), crop.top(), crop.bottom());

    if (top && left) {
        return Handle::TopLeft;
    }
    if (top && right) {
        return Handle::TopRight;
    }
    if (bottom && left) {
        return Handle::BottomLeft;
    }
    if (bottom && right) {
        return Handle::BottomRight;
    }
    if (left && alongY) {
        return Handle::Left;
    }
    if (right && alongY) {
        return Handle::Right;
    }
    if (top && alongX) {
        return Handle::Top;
    }
    if (bottom && alongX) {
        return Handle::Bottom;
    }
    // A crop of the whole photo has nowhere to move to, and it is what every
    // photo starts with: a drag on it draws the first crop instead.
    return crop.contains(position) && !isWholeImage(crop_) ? Handle::Move : Handle::None;
}

QPointF CropCanvas::toNormalised(const QPointF& position) const {
    const QRectF image = imageRect();
    if (image.isEmpty()) {
        return {};
    }
    // Not clamped: a drag that leaves the photo still moves the crop as far
    // as it goes, and the geometry stops it at the border.
    return {(position.x() - image.x()) / image.width(),
            (position.y() - image.y()) / image.height()};
}

void CropCanvas::updateCursor(Handle handle) {
    using enum Handle;
    switch (handle) {
    case None:
        setCursor(isReady() ? Qt::CrossCursor : Qt::ArrowCursor);
        break;
    case Move:
        setCursor(Qt::SizeAllCursor);
        break;
    case Left:
    case Right:
        setCursor(Qt::SizeHorCursor);
        break;
    case Top:
    case Bottom:
        setCursor(Qt::SizeVerCursor);
        break;
    case TopLeft:
    case BottomRight:
        setCursor(Qt::SizeFDiagCursor);
        break;
    case TopRight:
    case BottomLeft:
        setCursor(Qt::SizeBDiagCursor);
        break;
    }
}

void CropCanvas::requestPreview() {
    if (!preview_->hasSource() || size().isEmpty()) {
        return;
    }
    preview_->requestFitted(targetPixels());
}

void CropCanvas::onReadinessChanged(bool ready) {
    if (ready && awaitingFirstPreview_) {
        // Only now is there a size to measure the aspect against.
        awaitingFirstPreview_ = false;
        const std::optional<QRectF> remembered = pendingCrop_;
        pendingCrop_.reset();
        if (remembered) {
            setCropRect(*remembered);
        } else {
            resetCrop();
        }
        Q_EMIT cropChanged(crop_);
    }
    updateCursor(Handle::None);
    update();
}

void CropCanvas::changeCrop(const QRectF& rect) {
    if (rect == crop_) {
        return;
    }
    crop_ = rect;
    Q_EMIT cropChanged(crop_);
    update();
}

} // namespace cullfinch::cropper
