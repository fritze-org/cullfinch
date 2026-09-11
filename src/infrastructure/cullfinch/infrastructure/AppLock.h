// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>

#include <QLockFile>
#include <QString>

#include <memory>

namespace cullfinch::infrastructure {

/// Protects a collection against simultaneous Cullfinch writers.
///
/// The lock file lives beside the metadata database, never in the collection:
/// writing to the photo directory must never be a precondition for browsing
/// it. A second instance may still open the collection read-only. External
/// tools are outside this lock, which is exactly why every operation
/// revalidates its preconditions immediately before execution.
class AppLock final : public application::ICollectionLock {
public:
    AppLock() = default;
    ~AppLock() override;

    AppLock(const AppLock&) = delete;
    AppLock& operator=(const AppLock&) = delete;
    AppLock(AppLock&&) = delete;
    AppLock& operator=(AppLock&&) = delete;

    /// @return true when this process may write. On false, `holder` describes
    ///         the process that holds the lock, where the platform reports it.
    bool acquire(const QString& collectionRoot, QString* holder) override;
    [[nodiscard]] bool isHeld() const override { return held_; }
    void release() override;

    /// The lock file a collection root maps to. Exposed for tests.
    [[nodiscard]] static QString lockFileFor(const QString& collectionRoot);

private:
    std::unique_ptr<QLockFile> lock_;
    QString root_;
    bool held_ = false;
};

} // namespace cullfinch::infrastructure
