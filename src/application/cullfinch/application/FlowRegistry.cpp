// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/FlowRegistry.h>

namespace cullfinch::application {

bool FlowRegistry::registerFlow(const domain::FlowDescriptor& descriptor, FlowFactory factory) {
    if (!descriptor.isValid() || !factory) {
        return false;
    }
    if (contains(descriptor.id)) {
        return false;
    }
    entries_.append(Entry{descriptor, std::move(factory)});
    return true;
}

bool FlowRegistry::contains(const QString& flowId) const {
    for (const Entry& entry : entries_) {
        if (entry.descriptor.id == flowId) {
            return true;
        }
    }
    return false;
}

QList<domain::FlowDescriptor> FlowRegistry::descriptors() const {
    QList<domain::FlowDescriptor> result;
    result.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        result.append(entry.descriptor);
    }
    return result;
}

domain::FlowDescriptor FlowRegistry::descriptor(const QString& flowId) const {
    for (const Entry& entry : entries_) {
        if (entry.descriptor.id == flowId) {
            return entry.descriptor;
        }
    }
    return domain::FlowDescriptor{};
}

std::unique_ptr<domain::IComparisonFlow> FlowRegistry::create(const QString& flowId) const {
    for (const Entry& entry : entries_) {
        if (entry.descriptor.id == flowId) {
            return entry.factory();
        }
    }
    return nullptr;
}

} // namespace cullfinch::application
