// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/FlowView.h>

#include <algorithm>

namespace cullfinch::ui {

bool FlowViewRegistry::registerView(const QString& flowId, FlowViewFactory factory) {
    if (flowId.isEmpty() || !factory || contains(flowId)) {
        return false;
    }
    factories_.append({flowId, std::move(factory)});
    return true;
}

bool FlowViewRegistry::contains(const QString& flowId) const {
    return std::ranges::any_of(factories_,
                               [&flowId](const auto& entry) { return entry.first == flowId; });
}

std::unique_ptr<IFlowView> FlowViewRegistry::create(const QString& flowId,
                                                    application::IImageService& images) const {
    for (const auto& [id, factory] : factories_) {
        if (id == flowId) {
            return factory(images);
        }
    }
    return nullptr;
}

} // namespace cullfinch::ui
