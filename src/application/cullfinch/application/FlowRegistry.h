// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FlowContract.h>

#include <QList>
#include <QString>

#include <functional>
#include <memory>

namespace cullfinch::application {

/// Creates a fresh engine instance for one session.
using FlowFactory = std::function<std::unique_ptr<domain::IComparisonFlow>()>;

/// The registry the comparison menu is built from.
///
/// Adding a flow means registering a factory and a matching view under the same
/// stable identifier. It never requires another `switch` in the browser, the
/// pairing resolver or the operation executor.
class FlowRegistry {
public:
    /// @return false when the descriptor is invalid or the identifier is taken.
    bool registerFlow(const domain::FlowDescriptor& descriptor, FlowFactory factory);

    [[nodiscard]] bool contains(const QString& flowId) const;
    [[nodiscard]] QList<domain::FlowDescriptor> descriptors() const;
    [[nodiscard]] domain::FlowDescriptor descriptor(const QString& flowId) const;

    /// @return nullptr when the identifier is unknown.
    [[nodiscard]] std::unique_ptr<domain::IComparisonFlow> create(const QString& flowId) const;

private:
    struct Entry {
        domain::FlowDescriptor descriptor;
        FlowFactory factory;
    };
    QList<Entry> entries_;
};

} // namespace cullfinch::application
