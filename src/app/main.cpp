// SPDX-License-Identifier: GPL-3.0-or-later
#include <CompositionRoot.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QSqlDatabase>
#include <QTextStream>

#include <functional>

namespace {

/// Non-interactive verification of the *deployed* package.
///
/// It decodes a real JPEG, opens SQLite and shows both comparison flows, so a
/// missing image-format, platform or SQL driver plugin fails here rather than
/// in a user's hands. It is a smoke path, not a product feature.
int runSmoke(cullfinch::ui::BrowserWindow* window, cullfinch::app::CompositionRoot& root,
             const QString& directory) {
    QTextStream out(stdout);

    const auto fail = [&out](const QString& reason) {
        out << "smoke: failed - " << reason << Qt::endl;
        return 2;
    };

    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return fail(QStringLiteral("the SQLite driver plugin is missing"));
    }
    if (!window->openDirectory(directory)) {
        return fail(QStringLiteral("the directory could not be opened"));
    }

    const auto pump = [](const std::function<bool()>& done, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return done();
    };

    if (!pump([window]() { return window->model()->rowCount() > 0; }, 30000)) {
        return fail(QStringLiteral("the scan produced no photos"));
    }

    QList<cullfinch::domain::AssetId> everything;
    for (int row = 0; row < window->model()->rowCount(); ++row) {
        everything.append(window->model()->idForRow(row));
    }

    for (const QString& flowId : {QStringLiteral("image-wall"), QStringLiteral("versus-tree")}) {
        window->selectAssets(everything);
        if (!window->startFlow(flowId)) {
            return fail(QStringLiteral("flow '%1' did not start").arg(flowId));
        }
        cullfinch::ui::ComparisonShell* shell = window->activeShell();
        if (shell == nullptr) {
            return fail(QStringLiteral("flow '%1' produced no view").arg(flowId));
        }

        // Prove a real JPEG actually decoded through the deployed plugin.
        if (!pump([&root]() { return root.images().memoryUsedBytes() > 0; }, 30000)) {
            return fail(QStringLiteral("no image decoded for flow '%1'").arg(flowId));
        }

        QString error;
        if (!root.session().discard(&error)) {
            return fail(QStringLiteral("flow '%1' could not be discarded: %2").arg(flowId, error));
        }
        shell->close();
        QCoreApplication::processEvents();
    }

    // Naming the backend is the point on Linux: a package that only works
    // through XWayland does not satisfy the primary Linux target, and looks
    // identical to one that does unless it says which plugin it loaded.
    out << "smoke: platform=" << QGuiApplication::platformName() << Qt::endl;
    out << "smoke: ok" << Qt::endl;
    return 0;
}

} // namespace

namespace {

/// Report the backend actually in use, and say so plainly when it is not the
/// one this platform is built around.
///
/// A Wayland session that silently ends up on XCB through XWayland looks
/// identical to a working native run until something subtle misbehaves, so the
/// fallback is diagnosed rather than hidden. It is reported, never overridden:
/// an explicit `-platform` choice by the user is theirs to make.
void reportPlatformBackend() {
    const QString backend = QGuiApplication::platformName();
    qInfo().noquote() << QStringLiteral("cullfinch %1, Qt %2, platform plugin '%3'")
                             .arg(QStringLiteral(CULLFINCH_VERSION),
                                  QString::fromLatin1(qVersion()), backend);

    const bool waylandSession = qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
    if (waylandSession && backend != QLatin1String("wayland")) {
        qWarning().noquote()
            << QStringLiteral(
                   "cullfinch: this is a Wayland session but Qt selected the '%1' backend. "
                   "Rendering and scaling go through XWayland. Pass -platform wayland to "
                   "require the native path, or -platform xcb to silence this.")
                   .arg(backend);
    }
}

/// Options shared by the console and GUI startup paths.
struct CommandLine {
    QCommandLineParser parser;
    QCommandLineOption dataDirectory{
        QStringLiteral("data-dir"),
        QCoreApplication::translate("cullfinch",
                                    "Use an alternative directory for the Cullfinch database."),
        QStringLiteral("path")};
    QCommandLineOption cacheDirectory{
        QStringLiteral("cache-dir"),
        QCoreApplication::translate("cullfinch", "Use an alternative thumbnail cache directory."),
        QStringLiteral("path")};
    QCommandLineOption smoke{
        QStringLiteral("smoke"),
        QCoreApplication::translate("cullfinch",
                                    "Run a non-interactive package verification and exit.")};

    CommandLine() {
        parser.setApplicationDescription(
            QCoreApplication::translate("cullfinch", "Cull a directory of photos."));
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addPositionalArgument(
            QStringLiteral("directory"),
            QCoreApplication::translate("cullfinch", "Directory of photos to open."));
        parser.addOption(dataDirectory);
        parser.addOption(cacheDirectory);
        parser.addOption(smoke);
    }
};

/// True when the arguments only ask something that needs no display.
bool wantsConsoleOnly(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--version") || argument == QLatin1String("-v") ||
            argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            return true;
        }
    }
    return false;
}

void setApplicationIdentity() {
    QCoreApplication::setOrganizationName(QStringLiteral("cullfinch"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("cullfinch.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("cullfinch"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CULLFINCH_VERSION));
}

} // namespace

int main(int argc, char* argv[]) {
    // --version and --help must answer without a display. Building a
    // QApplication first would make them depend on a usable GUI platform
    // plugin, so a packaging check could not even ask which build it is
    // holding -- which is exactly how this was found.
    if (wantsConsoleOnly(argc, argv)) {
        QCoreApplication console(argc, argv);
        setApplicationIdentity();
        CommandLine commandLine;
        commandLine.parser.process(console);
        return 0;
    }

    QApplication application(argc, argv);
    setApplicationIdentity();
    // The identifiers above stay lowercase because paths derive from them;
    // what people read is the confirmed product name.
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Cullfinch"));

    // Matches the installed .desktop basename so the desktop can identify
    // Cullfinch for window grouping, the task switcher and its icon. Wayland
    // has no window-class fallback for this: without it the application shows
    // up unnamed and ungrouped.
    QGuiApplication::setDesktopFileName(QStringLiteral(CULLFINCH_DESKTOP_ID));

    CommandLine commandLine;
    QCommandLineParser& parser = commandLine.parser;
    parser.process(application);

    reportPlatformBackend();

    cullfinch::app::CompositionRoot::Options options;
    options.dataDirectory = parser.value(commandLine.dataDirectory);
    options.cacheDirectory = parser.value(commandLine.cacheDirectory);

    cullfinch::app::CompositionRoot root(options);
    QString error;
    if (!root.initialise(&error)) {
        if (parser.isSet(commandLine.smoke)) {
            QTextStream(stderr) << "smoke: failed - " << error << Qt::endl;
            return 2;
        }
        QMessageBox::critical(nullptr, QCoreApplication::translate("cullfinch", "Cullfinch"),
                              error);
        return 1;
    }

    cullfinch::ui::BrowserWindow* window = root.createBrowserWindow();

    // Clean shutdown waits for the draft. A quit that does not pass through
    // the shell's close event -- the File menu, a session logout, SIGTERM
    // handled by Qt -- would otherwise drop a coalesced autosave that had
    // not fired yet. The autosave is synchronous (decision 0005), so this
    // completes before exec() returns.
    QObject::connect(&application, &QCoreApplication::aboutToQuit, &application, [&root]() {
        QString flushError;
        if (!root.session().flushPendingSave(&flushError)) {
            qWarning().noquote()
                << QStringLiteral("cullfinch: the comparison draft could not be saved on exit: %1")
                       .arg(flushError);
        }
    });

    // Dialogs belong to the composition root: the browser reports, the
    // application decides how loudly. Automated runs install neither and so
    // never block on a prompt nobody can dismiss.
    QObject::connect(window, &cullfinch::ui::BrowserWindow::errorOccurred, window,
                     [window](const QString& message) {
                         QMessageBox::warning(window,
                                              QCoreApplication::translate("cullfinch", "Cullfinch"),
                                              message);
                     });

    window->setResumePrompt([window](const cullfinch::application::StoredSession& session) {
        const int answer = QMessageBox::question(
            window, QCoreApplication::translate("cullfinch", "Unfinished comparison"),
            QCoreApplication::translate(
                "cullfinch",
                "A saved comparison for this directory has %1 elimination(s) that were never "
                "applied.\n\nResume it, or discard the draft? Your existing deletion marks are "
                "unaffected either way.")
                .arg(session.draftRejected.size()),
            QMessageBox::Open | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Open);

        if (answer == QMessageBox::Open) {
            return cullfinch::ui::BrowserWindow::ResumeChoice::Resume;
        }
        if (answer == QMessageBox::Discard) {
            return cullfinch::ui::BrowserWindow::ResumeChoice::Discard;
        }
        return cullfinch::ui::BrowserWindow::ResumeChoice::Leave;
    });

    window->show();

    const QStringList arguments = parser.positionalArguments();

    if (parser.isSet(commandLine.smoke)) {
        if (arguments.isEmpty()) {
            QTextStream(stderr) << "smoke: failed - no directory argument" << Qt::endl;
            delete window;
            return 2;
        }
        const int result = runSmoke(window, root, QDir(arguments.first()).absolutePath());
        delete window;
        return result;
    }

    window->setAttribute(Qt::WA_DeleteOnClose, true);
    if (!arguments.isEmpty()) {
        window->openDirectory(QDir(arguments.first()).absolutePath());
    }

    return QApplication::exec();
}
