// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/cropper/CropGeometry.h>
#include <cullfinch/ui/AssetPresentation.h>
#include <cullfinch/ui/PreviewLoader.h>

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QWidget>

#include <optional>

namespace cullfinch::cropper {

/// One photo, fitted to the widget, with a crop rectangle to draw and adjust.
///
/// Not an `ImageCanvas`: there a click eliminates a photo, which is exactly
/// the gesture that draws a rectangle here. The decode side is the same
/// `PreviewLoader`, so the photo arrives the way it does everywhere else.
///
/// The rectangle is kept normalised to the photo as displayed. It is drawn on
/// a screen-sized preview and written from the full-resolution file, and only
/// a normalised rectangle means the same thing on both.
///
/// Keys are left to the window: the canvas takes no focus.
class CropCanvas : public QWidget {
    Q_OBJECT

public:
    explicit CropCanvas(application::IImageService& images, QWidget* parent = nullptr);

    /// Show another photo. `remembered` is a crop accepted for it earlier in
    /// this run, shown again on the way back; without one the crop starts as
    /// the largest of the current aspect.
    void setPhoto(const ui::AssetPresentation& presentation, quint64 generation,
                  const std::optional<QRectF>& remembered = std::nullopt);

    [[nodiscard]] ui::PreviewLoader* preview() const { return preview_; }
    [[nodiscard]] bool isReady() const { return preview_->isReady(); }

    [[nodiscard]] QRectF cropRect() const { return crop_; }
    /// Clamped into the photo; ignores the aspect, as a remembered crop must.
    void setCropRect(const QRectF& rect);
    /// Back to the largest crop of the current aspect.
    void resetCrop();

    [[nodiscard]] AspectChoice aspect() const { return aspect_; }
    /// Constrain the crop to `choice`, starting over at the largest crop of
    /// that shape near where the current one is. Free keeps the crop as it is.
    void setAspect(AspectChoice choice);

    /// The displayed photo's size in pixels, which ratios are measured
    /// against. Empty until a preview has arrived.
    [[nodiscard]] QSizeF imageSize() const;
    /// The rectangle the fitted photo occupies, in widget coordinates.
    [[nodiscard]] QRectF imageRect() const;
    /// The crop rectangle in widget coordinates.
    [[nodiscard]] QRectF cropWidgetRect() const;

    /// Device pixels a preview is asked for at, so a host can pre-load the
    /// next photo at exactly the size the image service will be asked for.
    [[nodiscard]] QSize targetPixels() const;

signals:
    void cropChanged(const QRectF& rect);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] Handle handleAt(const QPointF& position) const;
    [[nodiscard]] QPointF toNormalised(const QPointF& position) const;
    void updateCursor(Handle handle);
    void requestPreview();
    void onReadinessChanged(bool ready);
    void changeCrop(const QRectF& rect);

    ui::PreviewLoader* preview_ = nullptr;
    /// Waits for a resize to settle before asking for a preview at the new
    /// size; the old one is scaled meanwhile.
    QTimer* refinement_ = nullptr;

    QRectF crop_ = wholeImage();
    AspectChoice aspect_;
    /// A crop to show once the photo has arrived. Until then there is no size
    /// to measure an aspect against.
    std::optional<QRectF> pendingCrop_;
    bool awaitingFirstPreview_ = false;

    /// The pointer gesture in progress. `Handle::None` while active means a
    /// new rectangle is being drawn from `origin`.
    struct Drag {
        bool active = false;
        Handle handle = Handle::None;
        /// Where the press landed, in widget coordinates and normalised.
        QPoint pressPosition;
        QPointF origin;
        QRectF startRect;
    };
    Drag drag_;
};

} // namespace cullfinch::cropper
