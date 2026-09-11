// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/ImageCanvas.h>

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace cullfinch::ui {
namespace {

constexpr qreal kMaxZoom = 8.0;
constexpr qreal kMinZoom = 1.0;

} // namespace

ImageCanvas::ImageCanvas(application::IImageService& images, QWidget* parent)
    : QWidget(parent), images_(images) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(false);

    connect(&images_, &application::IImageService::imageReady, this, &ImageCanvas::onImageReady);
}

void ImageCanvas::setPresentation(const AssetPresentation& presentation, quint64 generation) {
    presentation_ = presentation;
    generation_ = generation;
    fitted_ = QImage();
    full_ = QImage();
    nativeSize_ = QSize();
    ready_ = false;
    fullResolutionReady_ = false;
    errorText_.clear();
    inspecting_ = false;
    centre_ = QPointF(0.5, 0.5);
    zoom_ = 1.0;

    updateAccessibility();
    Q_EMIT readinessChanged(false);
    update();

    if (!presentation_.previewPath.isEmpty()) {
        requestImage(application::ImageRequestClass::Comparison);
    }
}

void ImageCanvas::clearPresentation() {
    setPresentation(AssetPresentation{}, generation_);
}

void ImageCanvas::requestImage(application::ImageRequestClass kind) {
    application::ImageRequest request;
    request.memberId = presentation_.previewMemberId;
    request.path = presentation_.previewPath;
    request.fingerprint = presentation_.previewFingerprint;
    request.kind = kind;
    request.generation = generation_;

    if (kind == application::ImageRequestClass::Comparison) {
        // Screen-sized, accounting for the device pixel ratio so a fitted image
        // is never upscaled from too few pixels.
        const qreal ratio = devicePixelRatioF();
        request.targetSize =
            QSize(static_cast<int>(width() * ratio), static_cast<int>(height() * ratio));
        request.priority = 100;
        fitRequestId_ = images_.request(request);
    } else {
        request.targetSize = QSize(); // Native resolution.
        request.priority = 50;
        fullRequestId_ = images_.request(request);
    }
}

void ImageCanvas::onImageReady(const application::ImageResult& result) {
    if (result.generation != generation_ || !(result.memberId == presentation_.previewMemberId)) {
        return; // A superseded request, or another canvas's image.
    }

    if (!result.success) {
        // A decode error is a reported failure, never a rejection decision.
        errorText_ = result.error;
        ready_ = false;
        Q_EMIT readinessChanged(false);
        update();
        return;
    }

    nativeSize_ = result.nativeSize;
    if (result.requestId == fitRequestId_) {
        fitted_ = result.image;
        errorText_.clear();
        ready_ = true;
        Q_EMIT readinessChanged(true);
    } else if (result.requestId == fullRequestId_) {
        full_ = result.image;
        fullResolutionReady_ = true;
    }
    updateAccessibility();
    update();
}

void ImageCanvas::updateAccessibility() {
    // Rejection, focus and errors need text as well as colour.
    QString name = presentation_.displayName;
    if (name.isEmpty()) {
        name = tr("No photo");
    }
    if (rejected_) {
        name = tr("%1, eliminated").arg(name);
    }
    setAccessibleName(name);
    setAccessibleDescription(errorText_.isEmpty() ? presentation_.pairingText : errorText_);
    setToolTip(name);
}

QRectF ImageCanvas::imageRect() const {
    const QSize source = fitted_.isNull() ? nativeSize_ : fitted_.size();
    if (source.isEmpty() || width() <= 0 || height() <= 0) {
        return {};
    }
    const qreal scale = std::min(static_cast<qreal>(width()) / source.width(),
                                 static_cast<qreal>(height()) / source.height());
    const qreal drawWidth = source.width() * scale;
    const qreal drawHeight = source.height() * scale;
    return {(width() - drawWidth) / 2.0, (height() - drawHeight) / 2.0, drawWidth, drawHeight};
}

void ImageCanvas::setInspecting(bool inspecting) {
    if (inspecting_ == inspecting) {
        return;
    }
    inspecting_ = inspecting;
    if (inspecting_ && !fullResolutionReady_ && !presentation_.previewPath.isEmpty()) {
        requestImage(application::ImageRequestClass::FullResolution);
    }
    if (!inspecting_) {
        centre_ = QPointF(0.5, 0.5);
        zoom_ = 1.0;
    }
    setCursor(inspecting_ ? Qt::OpenHandCursor : Qt::ArrowCursor);
    Q_EMIT inspectToggled(inspecting_);
    update();
}

void ImageCanvas::setNormalisedView(const QPointF& centre, qreal zoom) {
    centre_ = centre;
    zoom_ = std::clamp(zoom, kMinZoom, kMaxZoom);
    update();
}

void ImageCanvas::setSelectionHighlighted(bool highlighted) {
    highlighted_ = highlighted;
    update();
}

void ImageCanvas::setRejected(bool rejected) {
    rejected_ = rejected;
    updateAccessibility();
    update();
}

void ImageCanvas::setCaption(const QString& caption) {
    caption_ = caption;
    update();
}

void ImageCanvas::focusOutEvent(QFocusEvent* event) {
    QWidget::focusOutEvent(event);
    // Alt-Tab, a workspace switch, a dialog or the screen locking all land
    // here. Whatever gesture was in flight is abandoned rather than completed
    // against a photo the user is no longer looking at.
    armed_ = false;
    dragging_ = false;
    update();
}

void ImageCanvas::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        armed_ = false;
        dragging_ = false;
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
        // Moving to a differently scaled output changes how many buffer pixels
        // this widget owns, so the decoded size that was right a moment ago no
        // longer is. Any gesture in flight is disarmed for the same reason a
        // resize disarms one.
        armed_ = false;
        if (!presentation_.previewPath.isEmpty()) {
            requestImage(application::ImageRequestClass::Comparison);
            if (inspecting_) {
                requestImage(application::ImageRequestClass::FullResolution);
            }
        }
    }
#endif
}

void ImageCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // A resize between press and release means the photo under the pointer may
    // not be the one that was pressed.
    armed_ = false;
    if (!presentation_.previewPath.isEmpty()) {
        // A larger widget needs more pixels; a later refinement must never swap
        // candidate identities, which is why the member id is part of the match.
        requestImage(application::ImageRequestClass::Comparison);
    }
}

void ImageCanvas::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());

    if (!errorText_.isEmpty()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                         tr("%1\n\nPress R to retry.").arg(errorText_));
    } else if (fitted_.isNull()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter,
                         presentation_.previewPath.isEmpty() ? tr("No photo") : tr("Loading…"));
    } else if (inspecting_ && !full_.isNull()) {
        // 100% inspection: the visible window into the native-resolution image.
        const qreal ratio = devicePixelRatioF();
        const QSizeF viewport(width() * ratio / zoom_, height() * ratio / zoom_);
        const QRectF source(std::clamp(centre_.x() * full_.width() - viewport.width() / 2.0, 0.0,
                                       std::max(0.0, full_.width() - viewport.width())),
                            std::clamp(centre_.y() * full_.height() - viewport.height() / 2.0, 0.0,
                                       std::max(0.0, full_.height() - viewport.height())),
                            std::min<qreal>(viewport.width(), full_.width()),
                            std::min<qreal>(viewport.height(), full_.height()));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(rect(), full_, source);
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(imageRect(), fitted_);
    }

    if (rejected_) {
        // Not colour alone: a cross plus a label.
        painter.setOpacity(0.55);
        painter.fillRect(rect(), palette().color(QPalette::Shadow));
        painter.setOpacity(1.0);
        painter.setPen(QPen(palette().color(QPalette::BrightText), 3));
        painter.drawLine(rect().topLeft(), rect().bottomRight());
        painter.drawLine(rect().topRight(), rect().bottomLeft());
        painter.drawText(rect(), Qt::AlignCenter, tr("Eliminated"));
    }

    if (!caption_.isEmpty()) {
        const QRect strip(0, height() - 24, width(), 24);
        painter.fillRect(strip, palette().color(QPalette::Base));
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(strip, Qt::AlignCenter, caption_);
    }

    if (inspecting_) {
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft,
                         fullResolutionReady_ ? tr("Inspecting at 100%")
                                              : tr("Inspecting — loading full resolution…"));
    }

    if (highlighted_ || hasFocus()) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), hasFocus() ? 3 : 2));
        painter.drawRect(rect().adjusted(1, 1, -2, -2));
    }
}

void ImageCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    armed_ = true;
    dragging_ = inspecting_;
    dragMoved_ = false;
    dragOrigin_ = event->pos();
    if (dragging_) {
        setCursor(Qt::ClosedHandCursor);
    }
    Q_EMIT gestureArmed(event->pos());
}

void ImageCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || full_.isNull()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->pos() - dragOrigin_;
    if (delta.manhattanLength() > 2) {
        dragMoved_ = true;
    }
    dragOrigin_ = event->pos();

    centre_.setX(std::clamp(centre_.x() - static_cast<qreal>(delta.x()) / (full_.width() / zoom_),
                            0.0, 1.0));
    centre_.setY(std::clamp(centre_.y() - static_cast<qreal>(delta.y()) / (full_.height() / zoom_),
                            0.0, 1.0));
    Q_EMIT viewChanged(centre_, zoom_);
    update();
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDragging = dragging_;
    const bool wasArmed = armed_;
    dragging_ = false;
    armed_ = false;
    if (inspecting_) {
        setCursor(Qt::OpenHandCursor);
    }

    // A gesture the desktop interrupted is not a decision.
    if (!wasArmed) {
        return;
    }
    // A pan gesture is not an elimination.
    if (wasDragging && dragMoved_) {
        return;
    }
    if (inspecting_) {
        return;
    }
    if (!ready_) {
        return; // Decision input stays disabled until the preview is ready.
    }
    if (!rect().contains(event->pos())) {
        return; // Released outside: the gesture no longer refers to this photo.
    }
    Q_EMIT eliminateRequested(ActivationSource::Pointer);
}

void ImageCanvas::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        // Holding a key must not reject a sequence of photos.
        event->accept();
        return;
    }

    switch (event->key()) {
    case Qt::Key_Space:
        setInspecting(!inspecting_);
        event->accept();
        return;
    case Qt::Key_R:
        if (!errorText_.isEmpty()) {
            errorText_.clear();
            requestImage(application::ImageRequestClass::Comparison);
            Q_EMIT retryRequested();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (ready_ && !inspecting_) {
            Q_EMIT eliminateRequested(ActivationSource::Keyboard);
            event->accept();
            return;
        }
        break;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void ImageCanvas::wheelEvent(QWheelEvent* event) {
    if (!inspecting_) {
        QWidget::wheelEvent(event);
        return;
    }
    const qreal steps = event->angleDelta().y() / 120.0;
    zoom_ = std::clamp(zoom_ * std::pow(1.2, steps), kMinZoom, kMaxZoom);
    Q_EMIT viewChanged(centre_, zoom_);
    update();
    event->accept();
}

} // namespace cullfinch::ui
