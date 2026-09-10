// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace cullfinch::infrastructure {

/// Platform locations used by cullfinch.
///
/// Every path is overridable so tests can run against an isolated application
/// data root, cache and settings without touching the developer's own.
class Paths {
public:
    /// Directory holding the metadata database. Never the collection itself:
    /// marking and comparing must not require write access to the photos.
    [[nodiscard]] static QString applicationDataDirectory();

    /// Disposable thumbnail cache. Contains no authoritative rejection state.
    [[nodiscard]] static QString cacheDirectory();

    [[nodiscard]] static QString databaseFile();

    /// Collection-local staging root, on the same filesystem as the sources and
    /// excluded from scanning.
    [[nodiscard]] static QString stagingRootFor(const QString& collectionRoot);

    /// The staging directory name, so the scanner can skip it.
    [[nodiscard]] static QString stagingDirectoryName();

    /// Redirect every location, for tests and for `--data-dir`.
    static void overrideRoots(const QString& dataDirectory, const QString& cacheDirectory);
    static void clearOverrides();

private:
    static QString ensureDirectory(const QString& path);
};

} // namespace cullfinch::infrastructure
