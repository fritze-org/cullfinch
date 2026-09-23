// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <QDir>
#include <QIcon>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

#include <cstring>

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

QString platformRequestedOnCommandLine(std::span<char* const> arguments) {
    QString requested;
    // argv[0] is the program, never an option; Qt starts after it too.
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const char* argument = arguments[index];
        // Qt folds a leading "--" to "-" before matching its own options.
        if (std::strncmp(argument, "--", 2) == 0) {
            ++argument;
        }
        if (std::strcmp(argument, "-platform") == 0 && index + 1 < arguments.size()) {
            ++index;
            requested = QString::fromLocal8Bit(arguments[index]);
        }
    }
    return requested;
}

bool platformFallbackDeservesWarning(const QString& backend, bool waylandSession,
                                     const QString& commandLinePlatform,
                                     const QString& environmentPlatform) {
    if (!waylandSession || backend == QLatin1String("wayland")) {
        return false;
    }
    const QString& requested =
        commandLinePlatform.isNull() ? environmentPlatform : commandLinePlatform;
    // The first entry is what was asked for; anything after it is what Qt may fall back to. Its
    // plugin name ends where the plugin's own options begin.
    const QString firstChoice =
        requested.section(QLatin1Char(';'), 0, 0).section(QLatin1Char(':'), 0, 0);
    return firstChoice.isEmpty() || firstChoice.compare(backend, Qt::CaseInsensitive) != 0;
}

} // namespace cullfinch::app
