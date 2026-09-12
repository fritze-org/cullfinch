// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace cullfinch::infrastructure {
namespace {

/// Test overrides for the two platform roots.
///
/// Held in function-local static variables rather than at namespace scope: the
/// state has to be mutable, and this way it is not a global and cannot be read
/// before it is initialised.
QString& dataOverride() {
    static QString value;
    return value;
}

QString& cacheOverride() {
    static QString value;
    return value;
}

} // namespace

QString Paths::ensureDirectory(const QString& path) {
    QDir().mkpath(path);
    return path;
}

QString Paths::applicationDataDirectory() {
    if (!dataOverride().isEmpty()) {
        return ensureDirectory(dataOverride());
    }
    return ensureDirectory(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

QString Paths::cacheDirectory() {
    if (!cacheOverride().isEmpty()) {
        return ensureDirectory(cacheOverride());
    }
    return ensureDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
}

QString Paths::databaseFile() {
    return applicationDataDirectory() + QLatin1String("/cullfinch.sqlite");
}

QString Paths::stagingDirectoryName() {
    return QStringLiteral(".cullfinch-staging");
}

QString Paths::stagingRootFor(const QString& collectionRoot) {
    return QDir(collectionRoot).absoluteFilePath(stagingDirectoryName());
}

void Paths::overrideRoots(const QString& dataDirectory, const QString& cacheDirectory) {
    dataOverride() = dataDirectory;
    cacheOverride() = cacheDirectory;
}

void Paths::clearOverrides() {
    dataOverride().clear();
    cacheOverride().clear();
}

} // namespace cullfinch::infrastructure
