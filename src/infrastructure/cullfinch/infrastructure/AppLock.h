// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLockFile>
#include <QString>

#include <memory>

namespace cullfinch::infrastructure {

/// Protects a collection against simultaneous cullfinch writers.
///
/// A second instance may still open the collection read-only. External tools
/// are outside this lock, which is exactly why every operation revalidates its
/// preconditions immediately before execution.
class AppLock {
public:
    explicit AppLock(const QString& collectionRoot);
    ~AppLock();

    AppLock(const AppLock&) = delete;
    AppLock& operator=(const AppLock&) = delete;
    AppLock(AppLock&&) = delete;
    AppLock& operator=(AppLock&&) = delete;

    /// @return true when this process may write. On false, `holder` describes
    ///         the process that holds the lock, where the platform reports it.
    bool acquire(QString* holder);
    [[nodiscard]] bool isHeld() const { return held_; }
    void release();

private:
    std::unique_ptr<QLockFile> lock_;
    bool held_ = false;
};

} // namespace cullfinch::infrastructure
