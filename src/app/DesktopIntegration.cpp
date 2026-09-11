// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <QIcon>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

namespace cullfinch::app {
namespace {

/// The prefix every AppImage runtime mounts itself under.
constexpr QLatin1String kAppImageMountPrefix("/tmp/.mount_");

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
    return path.startsWith(kAppImageMountPrefix);
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
