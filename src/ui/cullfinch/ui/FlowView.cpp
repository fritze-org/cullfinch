// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/FlowView.h>

namespace cullfinch::ui {

bool FlowViewRegistry::registerView(const QString& flowId, FlowViewFactory factory) {
    if (flowId.isEmpty() || !factory || contains(flowId)) {
        return false;
    }
    factories_.append({flowId, std::move(factory)});
    return true;
}

bool FlowViewRegistry::contains(const QString& flowId) const {
    for (const auto& entry : factories_) {
        if (entry.first == flowId) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<IFlowView> FlowViewRegistry::create(const QString& flowId,
                                                    application::IImageService& images) const {
    for (const auto& entry : factories_) {
        if (entry.first == flowId) {
            return entry.second(images);
        }
    }
    return nullptr;
}

} // namespace cullfinch::ui
