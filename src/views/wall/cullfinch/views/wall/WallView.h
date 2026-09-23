// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/flows/wall/WallFlow.h>
#include <cullfinch/flows/wall/WallLayout.h>
#include <cullfinch/ui/FlowView.h>
#include <cullfinch/ui/ImageCanvas.h>

#include <QCheckBox>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QRect>
#include <QWidget>

namespace cullfinch::views::wall {

/// Every selected candidate on one screen.
///
/// Tiles are real widgets, not graphics items, so keyboard focus, accessible
/// names and platform focus indicators come from the widget system rather than
/// from a bespoke accessibility bridge.
class WallSurface : public QWidget {
    Q_OBJECT

public:
    explicit WallSurface(application::IImageService& images, QWidget* parent = nullptr);

    /// @param positions one entry per wall position, in display order,
    ///        placeholders included: a placeholder occupies a cell so the
    ///        survivors around it do not move, and its tile stays on the wall
    ///        marked as eliminated.
    void setCandidates(const QList<flows::wall::WallSlot>& positions,
                       const ui::AssetPresentationMap& presentations, quint64 revision);
    /// The tile showing this candidate, placeholders included: an eliminated
    /// candidate whose cell is being held still has one.
    [[nodiscard]] ui::ImageCanvas* tileFor(const domain::AssetId& id) const;
    /// The candidates actually on the wall, without placeholders.
    [[nodiscard]] QList<domain::AssetId> order() const;
    [[nodiscard]] quint64 layoutRevision() const { return revision_; }

    /// Cells about `pixels` wide, in a grid that grows downwards for a scroll
    /// area to show, instead of every tile shrinking until all of them fit.
    ///
    /// Zero, the default, fits every candidate into the surface: the comparison
    /// wall's behaviour, where nothing may be scrolled out of sight. With a
    /// width set, only tiles on or near the screen decode, and a tile that is
    /// resized again waits for the size to settle before decoding again.
    void setTileWidth(int pixels);
    [[nodiscard]] int tileWidth() const { return tileWidth_; }

signals:
    /// Carries the layout revision the gesture began against.
    void eliminateRequested(const cullfinch::domain::AssetId& id, quint64 layoutRevision);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void relayout();
    /// Release the decode hold on tiles near the part of the surface a scroll
    /// area shows, and keep it on the rest.
    void updateDeferredLoading();
    /// The part of this surface its parent shows, in surface coordinates.
    [[nodiscard]] QRect shownArea() const;
    void recordAspect(const domain::AssetId& id);
    [[nodiscard]] bool acceptGesture(const domain::AssetId& id, const QPoint& pointer);
    /// Remember where tiles that are about to leave used to be, so a repeat
    /// click at those coordinates does not hit whatever moves in.
    void recordVanishedTiles(const QList<flows::wall::WallSlot>& positions);
    /// Drop the tiles whose candidates are no longer on the wall.
    void removeDepartedTiles();
    /// Create the tile for one candidate and wire its gestures.
    void addTile(const domain::AssetId& id);

    application::IImageService& images_;
    /// Every wall position, placeholders included.
    QList<flows::wall::WallSlot> positions_;
    ui::AssetPresentationMap presentations_;
    QHash<domain::AssetId, ui::ImageCanvas*> tiles_;
    /// Oriented image size per candidate, learned once its preview decodes.
    /// The initial layout uses a placeholder aspect; recording the real one
    /// exactly once per tile lets the grid settle without oscillating between
    /// "resize the cell" and "re-fit the image".
    QHash<domain::AssetId, QSizeF> aspects_;
    quint64 revision_ = 0;
    int tileWidth_ = 0;
    /// The minimum height this surface last asked for on its grid's behalf.
    int requestedHeight_ = 0;

    /// Where a tile used to be, and when it vanished. Repeat clicks inside that
    /// region are swallowed for the platform double-click interval.
    struct VanishedTile {
        QRect region;
        qint64 elapsedAtRemoval = 0;
    };
    QList<VanishedTile> vanished_;
    QElapsedTimer sinceStart_;
    /// Where the last accepted gesture landed, in surface coordinates. A
    /// click that lands somewhere else is a deliberate new target even
    /// inside a region a tile just left.
    QPoint lastGesturePosition_{-1, -1};

    /// The gesture currently in flight, captured at press: the layout
    /// revision it was made against and where the pointer was. The release
    /// reports against *these*, never against whatever the layout is by then.
    domain::AssetId armedTile_;
    quint64 armedRevision_ = 0;
    QPoint armedPosition_{-1, -1};
};

/// The wall flow's view: layout controls plus the tile surface.
class WallView final : public ui::IFlowView {
public:
    explicit WallView(application::IImageService& images);

    [[nodiscard]] QWidget* widget() override { return root_; }
    void setActionSink(ActionSink sink) override { sink_ = std::move(sink); }
    void setPresentations(const ui::AssetPresentationMap& presentations) override;
    void setState(const domain::FlowState& state, const domain::FlowSummary& summary) override;
    [[nodiscard]] QList<QWidget*> auxiliaryControls() override;
    void setFullscreenPresentation(bool fullscreen) override;

    [[nodiscard]] WallSurface* surface() { return surface_; }

private:
    QWidget* root_ = nullptr;
    WallSurface* surface_ = nullptr;
    QLabel* emptyLabel_ = nullptr;
    QCheckBox* fixedPositions_ = nullptr;
    QPushButton* compact_ = nullptr;

    ui::AssetPresentationMap presentations_;
    ActionSink sink_;
    quint64 revision_ = 0;
    bool updatingControls_ = false;
};

} // namespace cullfinch::views::wall
