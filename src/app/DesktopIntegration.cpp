// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <QDebug>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <array>
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

/// The options besides -platform whose next argument Qt takes as their value while it looks for
/// the platform, as QGuiApplicationPrivate::createPlatformIntegration() does. Missing one would
/// read `-platformtheme -platform xcb` as asking for xcb, which Qt does not.
///
/// Qt consumes -geometry, -title and -icon only when XCB is the build's default platform. They
/// are skipped regardless, because the two ways of being wrong are not equal: skipping one Qt did
/// not consume costs a warning for a choice that was made, while not skipping one it did consume
/// hides a fallback nobody asked for.
constexpr std::array kOptionsTakingAValue{
    "-platformpluginpath", "-platformtheme", "-qwindowgeometry", "-qwindowtitle",
    "-qwindowicon",        "-geometry",      "-title",           "-icon"};

bool takesAValue(const char* option) {
    return std::ranges::any_of(kOptionsTakingAValue, [option](const char* candidate) {
        return std::strcmp(option, candidate) == 0;
    });
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
        if (index + 1 >= arguments.size()) {
            break;
        }
        if (std::strcmp(argument, "-platform") == 0) {
            ++index;
            requested = QString::fromLocal8Bit(arguments[index]);
        } else if (takesAValue(argument)) {
            ++index;
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
    // The first entry is what was asked for; anything after it is what Qt may fall back to. Qt
    // drops empty entries, so ";xcb" asks for xcb. The plugin name then ends where the plugin's own
    // options begin -- and there an empty name stays empty, since what follows it is an option.
    const QString firstChoice = requested.section(QLatin1Char(';'), 0, 0, QString::SectionSkipEmpty)
                                    .section(QLatin1Char(':'), 0, 0);
    return firstChoice.isEmpty() || firstChoice.compare(backend, Qt::CaseInsensitive) != 0;
}

DesktopStartup::DesktopStartup(std::span<char* const> arguments)
    : commandLinePlatform_(platformRequestedOnCommandLine(arguments)) {
    preferPortalDialogs();
}

void DesktopStartup::reportBackend(const QString& program, const QString& version) const {
    const QString backend = QGuiApplication::platformName();
    qInfo().noquote() << QStringLiteral("%1 %2, Qt %3, platform plugin '%4'")
                             .arg(program, version, QString::fromLatin1(qVersion()), backend);

    if (platformFallbackDeservesWarning(backend, qEnvironmentVariableIsSet("WAYLAND_DISPLAY"),
                                        commandLinePlatform_,
                                        qEnvironmentVariable("QT_QPA_PLATFORM"))) {
        qWarning().noquote()
            << QStringLiteral(
                   "%1: this is a Wayland session but Qt selected the '%2' backend. "
                   "Rendering and scaling go through XWayland. Pass -platform wayland to "
                   "require the native path, or choose XCB explicitly (-platform xcb or "
                   "QT_QPA_PLATFORM=xcb) to silence this.")
                   .arg(program, backend);
    }
}

} // namespace cullfinch::app
