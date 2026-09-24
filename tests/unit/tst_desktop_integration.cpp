// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QRegularExpression>
#include <QTest>

#include <vector>

using namespace cullfinch::app;

namespace {

/// Sets an environment variable for one scope and restores it exactly afterwards; a null value
/// unsets it. The variables these tests touch are process-wide and read by Qt at startup, so a test
/// that left one set would be changing the environment of every test that follows it in this
/// binary.
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const QByteArray& value)
        : name_(name), present_(qEnvironmentVariableIsSet(name)), previous_(qgetenv(name)) {
        if (value.isNull()) {
            qunsetenv(name_);
        } else {
            qputenv(name_, value);
        }
    }

    ~ScopedEnvironmentVariable() {
        if (present_) {
            qputenv(name_, previous_);
        } else {
            qunsetenv(name_);
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable(ScopedEnvironmentVariable&&) = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable&&) = delete;

private:
    const char* name_;
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

/// The platform a command line asks for, given it as the words a shell would pass.
QString requestedBy(std::vector<QByteArray> words) {
    std::vector<char*> argv;
    argv.reserve(words.size());
    for (QByteArray& word : words) {
        argv.push_back(word.data());
    }
    return platformRequestedOnCommandLine(argv);
}

/// The warning the backend report prints for a fallback, whatever the backend under test is.
QRegularExpression fallbackWarning() {
    return QRegularExpression(
        QStringLiteral("Qt selected the '%1' backend.*choose XCB explicitly")
            .arg(QRegularExpression::escape(QGuiApplication::platformName())));
}

/// The line every report starts with, which both startup cases print and neither is about.
void ignoreBackendLine() {
    QTest::ignoreMessage(QtInfoMsg, QRegularExpression(QStringLiteral("^tst \\S+, Qt ")));
}

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

    void aFallbackNobodyAskedForIsReported();
    void anExplicitChoiceIsTheUsersToMake();
    void aFallbackListStillReportsTheFallback();
    void theCommandLineOverridesTheEnvironment();
    void nativeWaylandOrNoWaylandSessionIsNeverReported();
    void platformIsReadFromTheCommandLineAsQtReadsIt();

    void aStartupThatAskedForNothingReportsTheFallback();
    void aStartupGivenTheBackendOnItsCommandLineStaysQuiet();
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
        const ScopedEnvironmentVariable theme{"QT_QPA_PLATFORMTHEME", QByteArray()};
        preferPortalDialogs();
        QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"), QByteArrayLiteral("xdgdesktopportal"));
    }
    {
        const ScopedEnvironmentVariable theme{"QT_QPA_PLATFORMTHEME", QByteArrayLiteral("gtk3")};
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

void TestDesktopIntegration::aFallbackNobodyAskedForIsReported() {
    // The case decision 0007 cares about: a Wayland session that Qt quietly put on XCB. Nothing
    // requested reads back as null from argv and as empty from an unset variable.
    const QString xcb = QStringLiteral("xcb");
    QVERIFY(platformFallbackDeservesWarning(xcb, true, QString(), QString()));
    QVERIFY(platformFallbackDeservesWarning(xcb, true, QString(), QStringLiteral("")));
}

void TestDesktopIntegration::anExplicitChoiceIsTheUsersToMake() {
    // Telling someone to pass -platform xcb to silence the warning is only honest if it does.
    const QString xcb = QStringLiteral("xcb");
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, xcb, QString()));
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, QString(), xcb));
    // Qt matches plugin keys regardless of case, and options follow the name after a colon.
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, QStringLiteral("XCB"), QString()));
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, QString(), QStringLiteral("xcb:nomitshm")));
}

void TestDesktopIntegration::aFallbackListStillReportsTheFallback() {
    // A request is a fallback list. Having set one is not a choice of whatever Qt fell back to --
    // "wayland;xcb" ending up on xcb is precisely the silent fallback being diagnosed.
    const QString xcb = QStringLiteral("xcb");
    QVERIFY(platformFallbackDeservesWarning(xcb, true, QString(), QStringLiteral("wayland;xcb")));
    QVERIFY(platformFallbackDeservesWarning(xcb, true, QStringLiteral("wayland;xcb"), QString()));
    // Putting xcb first is asking for it.
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, QString(), QStringLiteral("xcb;wayland")));
    // Qt drops empty entries, so this asks for xcb first too.
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, QString(), QStringLiteral(";xcb")));
    // An empty plugin name is not made up from the options after it.
    QVERIFY(platformFallbackDeservesWarning(xcb, true, QString(), QStringLiteral(":xcb")));
}

void TestDesktopIntegration::theCommandLineOverridesTheEnvironment() {
    // As in Qt: -platform wins over QT_QPA_PLATFORM, so it is the one that says what was asked.
    const QString xcb = QStringLiteral("xcb");
    QVERIFY(platformFallbackDeservesWarning(xcb, true, QStringLiteral("wayland;xcb"), xcb));
    QVERIFY(!platformFallbackDeservesWarning(xcb, true, xcb, QStringLiteral("wayland")));
}

void TestDesktopIntegration::nativeWaylandOrNoWaylandSessionIsNeverReported() {
    QVERIFY(
        !platformFallbackDeservesWarning(QStringLiteral("wayland"), true, QString(), QString()));
    // Outside a Wayland session XCB is not a fallback from anything.
    QVERIFY(!platformFallbackDeservesWarning(QStringLiteral("xcb"), false, QString(), QString()));
    QVERIFY(!platformFallbackDeservesWarning(QStringLiteral("offscreen"), false, QString(),
                                             QStringLiteral("wayland;xcb")));
}

void TestDesktopIntegration::platformIsReadFromTheCommandLineAsQtReadsIt() {
    const QString xcb = QStringLiteral("xcb");
    QCOMPARE(requestedBy({"cullfinch", "-platform", "xcb", "/photos"}), xcb);
    // Qt accepts its own options with a double dash too.
    QCOMPARE(requestedBy({"cullfinch", "--platform", "xcb"}), xcb);
    // Qt keeps the last one it saw.
    QCOMPARE(requestedBy({"cullfinch", "-platform", "wayland", "-platform", "xcb"}), xcb);

    // Nothing asked for is null, not empty, so it cannot pass for an explicit empty request.
    QVERIFY(requestedBy({"cullfinch", "/photos"}).isNull());
    // A -platform with no value after it is ignored by Qt, so it asks for nothing.
    QVERIFY(requestedBy({"cullfinch", "-platform"}).isNull());
    // argv[0] is the program, whatever it happens to be called.
    QVERIFY(requestedBy({"-platform", "xcb"}).isNull());
    // An option that merely starts with the word is a different option.
    QVERIFY(requestedBy({"cullfinch", "-platformtheme", "xcb"}).isNull());

    // Qt's other options take the next argument as their value, even when it is "-platform", so
    // the platform named after it was never asked for. Believing it would hide the fallback.
    QVERIFY(requestedBy({"cullfinch", "-platformtheme", "-platform", "xcb"}).isNull());
    QVERIFY(requestedBy({"cullfinch", "--platformtheme", "-platform", "xcb"}).isNull());
    QVERIFY(requestedBy({"cullfinch", "-qwindowtitle", "-platform", "xcb"}).isNull());
    QCOMPARE(requestedBy({"cullfinch", "-platformtheme", "gtk3", "-platform", "xcb"}), xcb);
}

void TestDesktopIntegration::aStartupThatAskedForNothingReportsTheFallback() {
    // The suite's own backend is not wayland, so in a Wayland session it stands in for the XCB
    // fallback. Nothing requested it, which is the case the report exists for.
    const ScopedEnvironmentVariable session{"WAYLAND_DISPLAY", QByteArrayLiteral("wayland-test")};
    const ScopedEnvironmentVariable platform{"QT_QPA_PLATFORM", QByteArray()};
    const ScopedEnvironmentVariable theme{"QT_QPA_PLATFORMTHEME", QByteArray()};

    std::vector<QByteArray> words{"tst"};
    std::vector<char*> argv{words.front().data()};
    const DesktopStartup desktop{argv};
#ifdef Q_OS_LINUX
    // Constructing it is also what asks for portal dialogs, before the QApplication would read it.
    QCOMPARE(qgetenv("QT_QPA_PLATFORMTHEME"), QByteArrayLiteral("xdgdesktopportal"));
#endif

    ignoreBackendLine();
    QTest::ignoreMessage(QtWarningMsg, fallbackWarning());
    desktop.reportBackend(QStringLiteral("tst"), QStringLiteral("0"));
}

void TestDesktopIntegration::aStartupGivenTheBackendOnItsCommandLineStaysQuiet() {
    // The same session, but the backend was named on the command line. What the constructor read
    // from argv is what the report later decides with -- after the QApplication would have
    // removed it -- so this is the one place the two halves are seen working together.
    const ScopedEnvironmentVariable session{"WAYLAND_DISPLAY", QByteArrayLiteral("wayland-test")};
    const ScopedEnvironmentVariable platform{"QT_QPA_PLATFORM", QByteArray()};
    const ScopedEnvironmentVariable theme{"QT_QPA_PLATFORMTHEME", QByteArray()};

    std::vector<QByteArray> words{"tst", "-platform",
                                  QGuiApplication::platformName().toLocal8Bit()};
    std::vector<char*> argv;
    argv.reserve(words.size());
    for (QByteArray& word : words) {
        argv.push_back(word.data());
    }
    const DesktopStartup desktop{argv};

    ignoreBackendLine();
    QTest::failOnWarning(fallbackWarning());
    desktop.reportBackend(QStringLiteral("tst"), QStringLiteral("0"));
}

QTEST_MAIN(TestDesktopIntegration)
#include "tst_desktop_integration.moc"
