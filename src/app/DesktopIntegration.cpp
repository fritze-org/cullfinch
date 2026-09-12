// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <QDir>
#include <QIcon>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

namespace cullfinch::app {
namespace {

/// Where an AppImage runtime mounts itself: inside the temporary directory
/// this process would use, under a marked directory name. Read per call rather
/// than cached, because the answer is an environment variable and a startup
/// this short has no business caching one.
QString appImageMountPrefix() {
    return QDir::tempPath() + QLatin1String("/.mount_");
}

} // namespace

QByteArray platformThemePreference(const QByteArray& configured) {
    if (!configured.isEmpty()) {
        return {};
    }
    return QByteArrayLiteral("xdgdesktopportal");
}

void preferPortalDialogs() {
#ifdef Q_OS_LINUX
    const QByteArray preferred = platformThemePreference(qgetenv("QT_QPA_PLATFORMTHEME"));
    if (preferred.isEmpty()) {
        return;
    }
    qputenv("QT_QPA_PLATFORMTHEME", preferred);
#endif
}

bool isUnreachableIconThemePath(const QString& path) {
    return path.startsWith(appImageMountPrefix());
}

QStringList reachableIconThemePaths(const QStringList& searchPaths) {
    QStringList reachable;
    reachable.reserve(searchPaths.size());
    for (const QString& path : searchPaths) {
        if (!isUnreachableIconThemePath(path)) {
            reachable.append(path);
        }
    }
    return reachable;
}

void pruneUnreachableIconThemePaths() {
#ifdef Q_OS_LINUX
    const QStringList searchPaths = QIcon::themeSearchPaths();
    const QStringList reachable = reachableIconThemePaths(searchPaths);
    if (reachable.size() == searchPaths.size()) {
        return;
    }
    QIcon::setThemeSearchPaths(reachable);
#endif
}

} // namespace cullfinch::app
