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

    void setCandidates(const QList<domain::AssetId>& order,
                       const ui::AssetPresentationMap& presentations, quint64 revision);
    [[nodiscard]] ui::ImageCanvas* tileFor(const domain::AssetId& id) const;
    [[nodiscard]] QList<domain::AssetId> order() const { return order_; }
    [[nodiscard]] quint64 layoutRevision() const { return revision_; }

signals:
    /// Carries the layout revision the gesture began against.
    void eliminateRequested(const cullfinch::domain::AssetId& id, quint64 layoutRevision);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void relayout();
    [[nodiscard]] bool acceptGesture(const domain::AssetId& id);

    application::IImageService& images_;
    QList<domain::AssetId> order_;
    ui::AssetPresentationMap presentations_;
    QHash<domain::AssetId, ui::ImageCanvas*> tiles_;
    quint64 revision_ = 0;

    /// Where a tile used to be, and when it vanished. Repeat clicks inside that
    /// region are swallowed for the platform double-click interval.
    struct VanishedTile {
        QRect region;
        qint64 elapsedAtRemoval = 0;
    };
    QList<VanishedTile> vanished_;
    QElapsedTimer sinceStart_;
    QPoint lastGesturePosition_{-1, -1};
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
