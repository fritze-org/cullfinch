// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>

#include <QStringList>

namespace cullfinch::testsupport {

/// Records what would have been trashed and can be told to fail.
///
/// Normal GUI tests always use this: the user's real Trash is never enumerated
/// or emptied by the test suite.
class FakeTrashAdapter final : public application::ITrashAdapter {
public:
    bool moveToTrash(const QString& path, QString* resultingPath, QString* error) override;

    /// Fail the next `count` calls, to exercise the staged-but-not-trashed path.
    void failNextCalls(int count) { failuresRemaining_ = count; }
    /// Report no Trash path, which the platform is allowed to do.
    void setReportsPath(bool reports) { reportsPath_ = reports; }

    [[nodiscard]] const QStringList& trashed() const { return trashed_; }
    [[nodiscard]] int callCount() const { return callCount_; }

private:
    QStringList trashed_;
    int callCount_ = 0;
    int failuresRemaining_ = 0;
    bool reportsPath_ = true;
};

} // namespace cullfinch::testsupport
