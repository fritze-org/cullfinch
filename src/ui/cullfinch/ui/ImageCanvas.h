// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/ui/AssetPresentation.h>

#include <QImage>
#include <QPointF>
#include <QWidget>

namespace cullfinch::ui {

/// Displays one photo and reports the gestures that matter.
///
/// The primary click means "eliminate this photo" in every flow, so a plain
/// click is never also a zoom command. Inspection is a separate mode entered
/// with Space; while inspecting, click-and-drag pans and does not eliminate.
class ImageCanvas : public QWidget {
    Q_OBJECT

public:
    explicit ImageCanvas(application::IImageService& images, QWidget* parent = nullptr);

    void setPresentation(const AssetPresentation& presentation, quint64 generation);
    void clearPresentation();
    [[nodiscard]] const AssetPresentation& presentation() const { return presentation_; }

    /// True once a preview at the requested size has arrived. Decision input
    /// stays disabled until every required preview is ready.
    [[nodiscard]] bool isReady() const { return ready_; }
    [[nodiscard]] bool hasError() const { return !errorText_.isEmpty(); }
    [[nodiscard]] QString errorText() const { return errorText_; }

    /// True once pixels at 100% are available, which is what judging sharpness
    /// needs. Visible in the overlay.
    [[nodiscard]] bool isFullResolutionReady() const { return fullResolutionReady_; }

    [[nodiscard]] bool isInspecting() const { return inspecting_; }
    void setInspecting(bool inspecting);

    /// Rectangle the fitted image occupies, in widget coordinates.
    [[nodiscard]] QRectF imageRect() const;

    /// Normalised view centre and zoom, for the optional linked view.
    [[nodiscard]] QPointF normalisedCentre() const { return centre_; }
    [[nodiscard]] qreal zoom() const { return zoom_; }
    void setNormalisedView(const QPointF& centre, qreal zoom);

    void setSelectionHighlighted(bool highlighted);
    void setRejected(bool rejected);
    [[nodiscard]] bool isRejected() const { return rejected_; }

    /// Extra text drawn under the photo, such as "JPG + 1 RAW".
    void setCaption(const QString& caption);

signals:
    void eliminateRequested();
    void inspectToggled(bool inspecting);
    void viewChanged(const QPointF& centre, qreal zoom);
    void readinessChanged(bool ready);
    void retryRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void requestImage(application::ImageRequestClass kind);
    void onImageReady(const application::ImageResult& result);
    void updateAccessibility();

    application::IImageService& images_;
    AssetPresentation presentation_;
    quint64 generation_ = 0;
    quint64 fitRequestId_ = 0;
    quint64 fullRequestId_ = 0;

    QImage fitted_;
    QImage full_;
    QSize nativeSize_;
    bool ready_ = false;
    bool fullResolutionReady_ = false;
    QString errorText_;
    QString caption_;

    bool inspecting_ = false;
    bool dragging_ = false;
    bool dragMoved_ = false;
    QPoint dragOrigin_;
    QPointF centre_{0.5, 0.5};
    qreal zoom_ = 1.0;

    bool highlighted_ = false;
    bool rejected_ = false;
};

} // namespace cullfinch::ui
