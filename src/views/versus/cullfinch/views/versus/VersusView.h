// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/flows/versus/VersusFlow.h>
#include <cullfinch/ui/FlowView.h>
#include <cullfinch/ui/ImageCanvas.h>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace cullfinch::views::versus {

/// Two candidates, side by side, at equal available area.
///
/// Both panes fit the entire oriented image and never crop by default. Clicking
/// an image eliminates it; the explicit "Keep left" and "Keep right" buttons say
/// the same thing the other way round, so the meaning of a click never reverses
/// between flows.
class VersusView final : public ui::IFlowView {
public:
    explicit VersusView(application::IImageService& images);

    [[nodiscard]] QWidget* widget() override { return root_; }
    void setActionSink(ActionSink sink) override { sink_ = std::move(sink); }
    void setPresentations(const ui::AssetPresentationMap& presentations) override;
    void setState(const domain::FlowState& state, const domain::FlowSummary& summary) override;
    [[nodiscard]] QList<QWidget*> auxiliaryControls() override;
    void setFullscreenPresentation(bool fullscreen) override;

    /// Test and shell access to the panes.
    [[nodiscard]] ui::ImageCanvas* leftCanvas() { return left_; }
    [[nodiscard]] ui::ImageCanvas* rightCanvas() { return right_; }

private:
    void eliminate(const domain::AssetId& id);
    void updateDecisionAvailability();
    void applyLinkedView(const QPointF& centre, qreal zoom, ui::ImageCanvas* source);

    QWidget* root_ = nullptr;
    ui::ImageCanvas* left_ = nullptr;
    ui::ImageCanvas* right_ = nullptr;
    QLabel* matchLabel_ = nullptr;
    QLabel* survivorLabel_ = nullptr;
    QPushButton* keepLeft_ = nullptr;
    QPushButton* keepRight_ = nullptr;
    QCheckBox* linkViews_ = nullptr;

    ui::AssetPresentationMap presentations_;
    ActionSink sink_;
    quint64 revision_ = 0;
    /// The node the displayed pair belongs to. A decision naming a candidate
    /// from an earlier match is refused by the engine as well.
    int matchNode_ = -1;
    domain::AssetId leftId_;
    domain::AssetId rightId_;
    bool complete_ = false;
    bool applyingLinkedView_ = false;
};

} // namespace cullfinch::views::versus
