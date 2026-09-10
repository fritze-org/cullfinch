// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>

namespace cullfinch::infrastructure {

/// Trash through Qt, which uses the macOS APIs and the freedesktop mechanism.
///
/// The platform may legitimately not report the resulting Trash path. A failure
/// is reported as a failure: there is never an automatic fallback to permanent
/// deletion.
class QtTrashAdapter final : public application::ITrashAdapter {
public:
    bool moveToTrash(const QString& path, QString* resultingPath, QString* error) override;
};

} // namespace cullfinch::infrastructure
