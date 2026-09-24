// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/ui/AssetPresentation.h>
#include <cullfinch/ui/PreviewLoader.h>

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QTimer>
#include <QWidget>

namespace cullfinch::ui {

/// Displays one photo and reports the gestures that matter.
///
/// The primary click means "eliminate this photo" in every flow, so a plain
/// click is never also a zoom command. Inspection is a separate mode entered
/// with Space; while inspecting, click-and-drag pans and does not eliminate.
///
/// The pixels come from a `PreviewLoader` this canvas owns. Hosts that need to
/// know whether a photo has arrived, or what went wrong, ask that loader --
/// see `docs/decisions/0010-preview-loader.md`.
class ImageCanvas : public QWidget {
    Q_OBJECT

public:
    /// How an elimination was asked for. A pointer gesture is judged against
    /// what was under the pointer when it was pressed; a key has no press and
    /// acts on the tile as it is now.
    enum class ActivationSource { Pointer, Keyboard };
    Q_ENUM(ActivationSource)

    explicit ImageCanvas(application::IImageService& images, QWidget* parent = nullptr);

    void setPresentation(const AssetPresentation& presentation, quint64 generation);
    void clearPresentation();
    [[nodiscard]] const AssetPresentation& presentation() const { return presentation_; }

    /// The decode side of this canvas: readiness, errors and the decoded
    /// pixels. Hosts connect to `readinessChanged` here, because decision
    /// input stays disabled until every required preview is ready.
    [[nodiscard]] PreviewLoader* preview() const { return preview_; }

    [[nodiscard]] bool isInspecting() const { return inspecting_; }
    void setInspecting(bool inspecting);

    /// Rectangle the fitted image occupies, in widget coordinates.
    [[nodiscard]] QRectF imageRect() const;

    /// Normalised view centre and zoom, for the optional linked view.
    [[nodiscard]] QPointF normalisedCentre() const { return centre_; }
    [[nodiscard]] qreal zoom() const { return zoom_; }
    void setNormalisedView(const QPointF& centre, qreal zoom);

    /// Marks this photo as the one the flow is keeping, such as the versus
    /// survivor. Drawn as a border, so it reads alongside the platform focus
    /// indicator rather than replacing it.
    void setSelectionHighlighted(bool highlighted);
    [[nodiscard]] bool isSelectionHighlighted() const { return highlighted_; }

    /// Marks this photo as eliminated. A host that leaves an eliminated photo
    /// on screen -- the wall's fixed-position placeholder does -- needs the
    /// mark to be legible, which is why it is a cross and a label and not a
    /// colour.
    void setRejected(bool rejected);
    [[nodiscard]] bool isRejected() const { return rejected_; }

    /// Extra text drawn under the photo, such as "JPG + 1 RAW".
    void setCaption(const QString& caption);

    /// Hold back decode requests while `deferred`.
    ///
    /// A host showing more photos than fit on screen -- the standalone wall,
    /// scrolled -- asks only for the ones somebody can see, rather than
    /// decoding a whole directory up front. Releasing the hold asks for a
    /// preview at the current size unless one was already asked for.
    void setLoadingDeferred(bool deferred);
    [[nodiscard]] bool isLoadingDeferred() const { return loadingDeferred_; }

    /// Gives the decoded pixels back, keeping the photo.
    ///
    /// The counterpart to holding a decode back: a host showing far more
    /// photos than fit on screen -- the standalone wall, pre-loading a whole
    /// directory -- would otherwise keep every image it has ever scrolled
    /// past. What it draws until the pixels return is the same placeholder it
    /// drew before they first arrived, and lifting the hold asks again, which
    /// the image service usually answers from its cache.
    ///
    /// Readiness drops with the pixels, so a host that gates decision input on
    /// readiness -- the comparison wall and the versus panes do -- must not
    /// call this for a photo somebody could be deciding about. A photo being
    /// inspected keeps its pixels whatever the host asks.
    void releasePixels();

    /// Wait this long after the last resize before asking for pixels at the
    /// new size, while a preview is already on screen. Meanwhile the preview
    /// already on screen is scaled.
    ///
    /// Zero, the default, asks on every resize. A host that resizes all of its
    /// canvases continuously -- a tile-size slider being dragged -- would
    /// otherwise queue a decode per canvas per step, each at a size nobody
    /// looks at for longer than a frame. A canvas with nothing on screen yet
    /// always asks at once: there is nothing to scale in the meantime.
    void setRefinementDelay(int milliseconds);

signals:
    /// A primary-button press that could become an elimination, with the
    /// pointer position in this widget's coordinates. Hosts bind the gesture
    /// to what was under the pointer *now*; the release reports only that
    /// the gesture completed.
    void gestureArmed(const QPoint& position);
    void eliminateRequested(cullfinch::ui::ImageCanvas::ActivationSource source);
    void inspectToggled(bool inspecting);
    void viewChanged(const QPointF& centre, qreal zoom);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    /// Screen-sized, accounting for the device pixel ratio so a fitted image is
    /// never upscaled from too few pixels. This is the one thing the loader
    /// cannot work out for itself, which is why it is asked for in pixels.
    [[nodiscard]] QSize fittedTargetPixels() const;
    void requestFittedPreview();
    void updateAccessibility();

    PreviewLoader* preview_ = nullptr;
    /// Created on first use: only hosts that set a refinement delay need one.
    QTimer* refinement_ = nullptr;
    int refinementDelayMs_ = 0;
    bool loadingDeferred_ = false;
    /// The size the current photo was last asked for, so releasing a deferred
    /// load does not ask again for what is already on its way.
    QSize requestedPixels_;
    AssetPresentation presentation_;
    QString caption_;

    bool inspecting_ = false;

    /// The pointer gesture in progress, if any.
    ///
    /// `armed` is true between press and release of a gesture that could still
    /// eliminate. Losing focus, changing scale or being resized disarms it: a
    /// decision must come from a gesture the user completed on the photo they
    /// were looking at, not one interrupted by the desktop.
    struct Gesture {
        bool armed = false;
        bool dragging = false;
        bool moved = false;
        QPoint origin;
    };
    Gesture gesture_;

    QPointF centre_{0.5, 0.5};
    qreal zoom_ = 1.0;

    bool highlighted_ = false;
    bool rejected_ = false;
};

} // namespace cullfinch::ui
