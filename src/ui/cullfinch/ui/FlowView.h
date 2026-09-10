// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/domain/FlowContract.h>
#include <cullfinch/ui/AssetPresentation.h>

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>

namespace cullfinch::ui {

/// The presentation half of a comparison flow.
///
/// A view produces validated actions and renders flow state. Each view can use
/// its own layout and controls while reusing the image canvas, the loading
/// service and the shared action dispatch. It never persists anything and never
/// deletes a file.
class IFlowView {
public:
    /// Every action carries the state revision the view was rendering, so a
    /// gesture begun against an older layout is rejected rather than applied.
    using ActionSink =
        std::function<void(const QString& name, const QJsonObject& payload, quint64 revision)>;

    IFlowView() = default;
    virtual ~IFlowView() = default;
    IFlowView(const IFlowView&) = delete;
    IFlowView& operator=(const IFlowView&) = delete;
    IFlowView(IFlowView&&) = delete;
    IFlowView& operator=(IFlowView&&) = delete;

    [[nodiscard]] virtual QWidget* widget() = 0;
    virtual void setActionSink(ActionSink sink) = 0;
    virtual void setPresentations(const AssetPresentationMap& presentations) = 0;
    virtual void setState(const domain::FlowState& state, const domain::FlowSummary& summary) = 0;

    /// Extra controls the shell puts in its strip, such as "Keep left".
    [[nodiscard]] virtual QList<QWidget*> auxiliaryControls() { return {}; }

    virtual void setFullscreenPresentation(bool fullscreen) { Q_UNUSED(fullscreen) }
};

using FlowViewFactory = std::function<std::unique_ptr<IFlowView>(application::IImageService&)>;

/// Views register under the same stable identifier as their engine, so the
/// shell can pair them without a switch statement.
class FlowViewRegistry {
public:
    bool registerView(const QString& flowId, FlowViewFactory factory);
    [[nodiscard]] bool contains(const QString& flowId) const;
    [[nodiscard]] std::unique_ptr<IFlowView> create(const QString& flowId,
                                                    application::IImageService& images) const;

private:
    QList<std::pair<QString, FlowViewFactory>> factories_;
};

} // namespace cullfinch::ui
