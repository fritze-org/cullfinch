// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <QDir>
#include <QIcon>
#include <QTest>

using namespace cullfinch::app;

namespace {

/// Restores QT_QPA_PLATFORMTHEME, whatever the test did to it. The variable is
/// process-wide and read by Qt at startup, so a test that left it set would be
/// changing the environment of every test that follows it in this binary.
class ScopedPlatformTheme {
public:
    explicit ScopedPlatformTheme(const QByteArray& value)
        : present_(qEnvironmentVariableIsSet(kName)), previous_(qgetenv(kName)) {
        if (value.isNull()) {
            qunsetenv(kName);
        } else {
            qputenv(kName, value);
        }
    }

    ~ScopedPlatformTheme() {
        if (present_) {
            qputenv(kName, previous_);
        } else {
            qunsetenv(kName);
        }
    }

    ScopedPlatformTheme(const ScopedPlatformTheme&) = delete;
    ScopedPlatformTheme& operator=(const ScopedPlatformTheme&) = delete;
    ScopedPlatformTheme(ScopedPlatformTheme&&) = delete;
    ScopedPlatformTheme& operator=(ScopedPlatformTheme&&) = delete;

private:
    static constexpr const char* kName = "QT_QPA_PLATFORMTHEME";

    bool present_;
    QByteArray previous_;
};

/// Restores QIcon's search paths for the same reason.
class ScopedIconThemePaths {
public:
    explicit ScopedIconThemePaths(const QStringList& paths) : previous_(QIcon::themeSearchPaths()) {
        QIcon::setThemeSearchPaths(paths);
    }

    ~ScopedIconThemePaths() { QIcon::setThemeSearchPaths(previous_); }

    ScopedIconThemePaths(const ScopedIconThemePaths&) = delete;
    ScopedIconThemePaths& operator=(const ScopedIconThemePaths&) = delete;
    ScopedIconThemePaths(ScopedIconThemePaths&&) = delete;
    ScopedIconThemePaths& operator=(ScopedIconThemePaths&&) = delete;

private:
    QStringList previous_;
};

} // namespace

class TestDesktopIntegration : public QObject {
    Q_OBJECT

private slots:
    void anUnsetPlatformThemeAsksForThePortal();
    void anExplicitPlatformThemeIsTheUsersChoice();
    void portalPreferenceIsAppliedOnlyWhereNothingIsConfigured();

    void anAppImageMountIsNotOursToSearch();
    void reachablePathsKeepTheirOrderAndNothingElse();
    void pruningLeavesTheSearchPathsWithoutTheMount();
};

void TestDesktopIntegration::anUnsetPlatformThemeAsksForThePortal() {
    // The case this exists for: nothing configured, so the portal is the
    // preference. An empty value is no configuration either -- it is what an
    // unset variable reads back as.
    QCOMPARE(platformThemePreference(QByteArray()), QByteArrayLiteral("xdgdesktopportal"));
    QCOMPARE(platformThemePreference(QByteArrayLiteral("")), QByteArrayLiteral("xdgdesktopportal"));
}

void TestDesktopIntegration::anExplicitPlatformThemeIsTheUsersChoice() {
    // Null means "leave it alone", and that includes a value naming the theme
    // we would have asked for: the user having set it is the point.
    QVERIFY(platformThemePreference(QByteArrayLiteral("gtk3")).isNull());
    QVERIFY(platformThemePreference(QByteArrayLiteral("xdgdesktopportal")).isNull());
}

void TestDesktopIntegration::portalPreferenceIsAppliedOnlyWhereNothingIsConfigured() {
#ifndef Q_OS_LINUX
    QSKIP("the platform theme plugin this chooses exists only on Linux");
#else
    {
        const ScopedPlatformTheme theme{QByteArray()};
        preferPortalDialogs();
        QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"), QByteArrayLiteral("xdgdesktopportal"));
    }
    {
        const ScopedPlatformTheme theme{QByteArrayLiteral("gtk3")};
        preferPortalDialogs();
        QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"), QByteArrayLiteral("gtk3"));
    }
#endif
}

void TestDesktopIntegration::anAppImageMountIsNotOursToSearch() {
    // The temporary directory this process would use, which is where an
    // AppImage launched in the same environment mounted itself -- not a
    // hardcoded /tmp, which TMPDIR overrides and macOS never uses.
    const QString temporary = QDir::tempPath();

    QVERIFY(isUnreachableIconThemePath(temporary + QStringLiteral("/.mount_kittyAbc/share/icons")));
    // Only that. A directory a user genuinely put in the temporary directory,
    // and one that merely contains the mount point further along, both stay.
    QVERIFY(!isUnreachableIconThemePath(QStringLiteral("/usr/share/icons")));
    QVERIFY(!isUnreachableIconThemePath(temporary + QStringLiteral("/icons")));
    QVERIFY(!isUnreachableIconThemePath(QStringLiteral("/home/me") + temporary +
                                        QStringLiteral("/.mount_x/share/icons")));
    QVERIFY(!isUnreachableIconThemePath(QString()));
}

void TestDesktopIntegration::reachablePathsKeepTheirOrderAndNothingElse() {
    const QString mount = QDir::tempPath() + QStringLiteral("/.mount_termAbc/share/");
    const QStringList searchPaths{
        mount + QStringLiteral("icons"), QStringLiteral("/home/me/.local/share/icons"),
        QStringLiteral("/usr/share/icons"), mount + QStringLiteral("pixmaps")};

    QCOMPARE(reachableIconThemePaths(searchPaths),
             QStringList({QStringLiteral("/home/me/.local/share/icons"),
                          QStringLiteral("/usr/share/icons")}));

    // Nothing to drop is the common case, and it must not reorder or lose.
    const QStringList clean{QStringLiteral("/usr/share/icons"), QStringLiteral("/usr/share/foo")};
    QCOMPARE(reachableIconThemePaths(clean), clean);
    QVERIFY(reachableIconThemePaths(QStringList()).isEmpty());
}

void TestDesktopIntegration::pruningLeavesTheSearchPathsWithoutTheMount() {
#ifndef Q_OS_LINUX
    QSKIP("only Linux hands us another program's AppImage mount");
#else
    const QStringList kept{QStringLiteral("/usr/share/icons")};
    {
        const ScopedIconThemePaths paths{
            QStringList({QDir::tempPath() + QStringLiteral("/.mount_termAbc/share/icons")}) + kept};
        pruneUnreachableIconThemePaths();
        QCOMPARE(QIcon::themeSearchPaths(), kept);
    }
    {
        // Nothing to prune leaves the paths exactly as Qt built them.
        const ScopedIconThemePaths paths{kept};
        pruneUnreachableIconThemePaths();
        QCOMPARE(QIcon::themeSearchPaths(), kept);
    }
#endif
}

QTEST_MAIN(TestDesktopIntegration)
#include "tst_desktop_integration.moc"
