// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace cullfinch::infrastructure {
namespace {

QString g_dataOverride;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
QString g_cacheOverride; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

} // namespace

QString Paths::ensureDirectory(const QString& path) {
    QDir().mkpath(path);
    return path;
}

QString Paths::applicationDataDirectory() {
    if (!g_dataOverride.isEmpty()) {
        return ensureDirectory(g_dataOverride);
    }
    return ensureDirectory(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
}

QString Paths::cacheDirectory() {
    if (!g_cacheOverride.isEmpty()) {
        return ensureDirectory(g_cacheOverride);
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
    g_dataOverride = dataDirectory;
    g_cacheOverride = cacheDirectory;
}

void Paths::clearOverrides() {
    g_dataOverride.clear();
    g_cacheOverride.clear();
}

} // namespace cullfinch::infrastructure
