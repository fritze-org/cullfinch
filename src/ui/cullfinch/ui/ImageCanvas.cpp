// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/ImageCanvas.h>

#include <QEvent>
#include <QFocusEvent>
#include <QImage>
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
    : QWidget(parent), preview_(new PreviewLoader(images, this)) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(64, 64);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(false);

    // Whatever the loader reports -- new pixels, a decode failure, a cleared
    // error -- is something drawn or described here.
    connect(preview_, &PreviewLoader::changed, this, [this]() {
        updateAccessibility();
        update();
    });
}

void ImageCanvas::setPresentation(const AssetPresentation& presentation, quint64 generation) {
    presentation_ = presentation;
    inspecting_ = false;
    centre_ = QPointF(0.5, 0.5);
    zoom_ = 1.0;
    // A different photo arrives unmarked. Its host re-applies whatever the flow
    // says about it, and a stale mark would otherwise describe the photo that
    // used to be here.
    highlighted_ = false;
    rejected_ = false;

    // Nothing asked for the previous photo is still wanted, at any size.
    requestedPixels_ = QSize();
    if (refinement_ != nullptr) {
        refinement_->stop();
    }

    // Sets this canvas's own state first: the loader reports readiness as it
    // takes the new source, and a host that answers that signal by asking what
    // is on screen must not be told about the photo that just left.
    preview_->setSource(presentation_, generation);

    updateAccessibility();
    update();

    if (preview_->hasSource()) {
        requestFittedPreview();
    }
}

void ImageCanvas::clearPresentation() {
    setPresentation(AssetPresentation{}, preview_->generation());
}

QSize ImageCanvas::fittedTargetPixels() const {
    const qreal ratio = devicePixelRatioF();
    return {static_cast<int>(width() * ratio), static_cast<int>(height() * ratio)};
}

void ImageCanvas::requestFittedPreview() {
    if (loadingDeferred_) {
        return; // Asked for when the host releases the hold.
    }
    if (refinement_ != nullptr) {
        refinement_->stop();
    }
    // A larger widget needs more pixels; a later refinement must never swap
    // candidate identities, which is why the member id is part of the match the
    // loader makes.
    requestedPixels_ = fittedTargetPixels();
    preview_->requestFitted(requestedPixels_);
}

void ImageCanvas::setLoadingDeferred(bool deferred) {
    if (loadingDeferred_ == deferred) {
        return; // Hosts re-apply this whenever they scroll.
    }
    loadingDeferred_ = deferred;
    if (loadingDeferred_ || !preview_->hasSource() || requestedPixels_ == fittedTargetPixels()) {
        return;
    }
    if (refinementDelayMs_ > 0 && !preview_->fitted().isNull()) {
        // Back in view at a size it was resized to while held: the same wait
        // as a resize in view, so a canvas scrolled past mid-drag does not ask
        // for every intermediate size either.
        refinement_->start(refinementDelayMs_);
        return;
    }
    requestFittedPreview();
}

void ImageCanvas::setRefinementDelay(int milliseconds) {
    refinementDelayMs_ = std::max(0, milliseconds);
    if (refinementDelayMs_ > 0 && refinement_ == nullptr) {
        refinement_ = new QTimer(this);
        refinement_->setSingleShot(true);
        connect(refinement_, &QTimer::timeout, this, [this]() {
            if (preview_->hasSource()) {
                requestFittedPreview();
            }
        });
    }
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
    setAccessibleDescription(preview_->hasError() ? preview_->errorText()
                                                  : presentation_.pairingText);
    setToolTip(name);
}

QRectF ImageCanvas::imageRect() const {
    const QSize source =
        preview_->fitted().isNull() ? preview_->nativeSize() : preview_->fitted().size();
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
    if (inspecting_ && !preview_->isFullResolutionReady() && preview_->hasSource()) {
        preview_->requestFullResolution();
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
    if (highlighted_ == highlighted) {
        return; // Hosts re-apply this on every state update.
    }
    highlighted_ = highlighted;
    update();
}

void ImageCanvas::setRejected(bool rejected) {
    if (rejected_ == rejected) {
        return; // Hosts re-apply this on every state update.
    }
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
    gesture_.armed = false;
    gesture_.dragging = false;
    update();
}

void ImageCanvas::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
        gesture_.armed = false;
        gesture_.dragging = false;
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    if (event->type() == QEvent::DevicePixelRatioChange) {
        // Moving to a differently scaled output changes how many buffer pixels
        // this widget owns, so the decoded size that was right a moment ago no
        // longer is. Any gesture in flight is disarmed for the same reason a
        // resize disarms one.
        gesture_.armed = false;
        if (preview_->hasSource()) {
            requestFittedPreview();
            if (inspecting_) {
                preview_->requestFullResolution();
            }
        }
    }
#endif
}

void ImageCanvas::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // A resize between press and release means the photo under the pointer may
    // not be the one that was pressed.
    gesture_.armed = false;
    if (!preview_->hasSource()) {
        return;
    }
    if (refinementDelayMs_ > 0 && !preview_->fitted().isNull()) {
        // The preview on screen is scaled until the size stops changing.
        refinement_->start(refinementDelayMs_);
        return;
    }
    requestFittedPreview();
}

void ImageCanvas::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(this);
    painter.fillRect(rect(), palette().window());

    const QImage& fitted = preview_->fitted();
    const QImage& full = preview_->full();

    if (preview_->hasError()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                         tr("%1\n\nPress R to retry.").arg(preview_->errorText()));
    } else if (fitted.isNull()) {
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter,
                         preview_->hasSource() ? tr("Loading…") : tr("No photo"));
    } else if (inspecting_ && !full.isNull()) {
        // 100% inspection: the visible window into the native-resolution image.
        const qreal ratio = devicePixelRatioF();
        const QSizeF viewport(width() * ratio / zoom_, height() * ratio / zoom_);
        const QRectF source(std::clamp(centre_.x() * full.width() - viewport.width() / 2.0, 0.0,
                                       std::max(0.0, full.width() - viewport.width())),
                            std::clamp(centre_.y() * full.height() - viewport.height() / 2.0, 0.0,
                                       std::max(0.0, full.height() - viewport.height())),
                            std::min<qreal>(viewport.width(), full.width()),
                            std::min<qreal>(viewport.height(), full.height()));
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(rect(), full, source);
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(imageRect(), fitted);
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
                         preview_->isFullResolutionReady()
                             ? tr("Inspecting at 100%")
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
    gesture_.armed = true;
    gesture_.dragging = inspecting_;
    gesture_.moved = false;
    gesture_.origin = event->pos();
    if (gesture_.dragging) {
        setCursor(Qt::ClosedHandCursor);
    }
    Q_EMIT gestureArmed(event->pos());
}

void ImageCanvas::mouseMoveEvent(QMouseEvent* event) {
    const QImage& full = preview_->full();
    if (!gesture_.dragging || full.isNull()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QPoint delta = event->pos() - gesture_.origin;
    if (delta.manhattanLength() > 2) {
        gesture_.moved = true;
    }
    gesture_.origin = event->pos();

    centre_.setX(
        std::clamp(centre_.x() - static_cast<qreal>(delta.x()) / (full.width() / zoom_), 0.0, 1.0));
    centre_.setY(std::clamp(centre_.y() - static_cast<qreal>(delta.y()) / (full.height() / zoom_),
                            0.0, 1.0));
    Q_EMIT viewChanged(centre_, zoom_);
    update();
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDragging = gesture_.dragging;
    const bool wasArmed = gesture_.armed;
    gesture_.dragging = false;
    gesture_.armed = false;
    if (inspecting_) {
        setCursor(Qt::OpenHandCursor);
    }

    // A gesture the desktop interrupted is not a decision.
    if (!wasArmed) {
        return;
    }
    // A pan gesture is not an elimination.
    if (wasDragging && gesture_.moved) {
        return;
    }
    if (inspecting_) {
        return;
    }
    if (!preview_->isReady()) {
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
        if (preview_->retryFitted(fittedTargetPixels())) {
            event->accept();
            return;
        }
        break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (preview_->isReady() && !inspecting_) {
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
