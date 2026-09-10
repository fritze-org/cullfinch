// SPDX-License-Identifier: GPL-3.0-or-later
#include <CompositionRoot.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
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

    out << "smoke: ok" << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("cullfinch"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("cullfinch.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("cullfinch"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CULLFINCH_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QCoreApplication::translate("cullfinch", "Cull a directory of photos."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("directory"),
        QCoreApplication::translate("cullfinch", "Directory of photos to open."));

    const QCommandLineOption dataDirectoryOption(
        QStringLiteral("data-dir"),
        QCoreApplication::translate("cullfinch",
                                    "Use an alternative directory for the cullfinch database."),
        QStringLiteral("path"));
    parser.addOption(dataDirectoryOption);

    const QCommandLineOption cacheDirectoryOption(
        QStringLiteral("cache-dir"),
        QCoreApplication::translate("cullfinch", "Use an alternative thumbnail cache directory."),
        QStringLiteral("path"));
    parser.addOption(cacheDirectoryOption);

    const QCommandLineOption smokeOption(
        QStringLiteral("smoke"),
        QCoreApplication::translate("cullfinch",
                                    "Run a non-interactive package verification and exit."));
    parser.addOption(smokeOption);

    parser.process(application);

    cullfinch::app::CompositionRoot::Options options;
    options.dataDirectory = parser.value(dataDirectoryOption);
    options.cacheDirectory = parser.value(cacheDirectoryOption);

    cullfinch::app::CompositionRoot root(options);
    QString error;
    if (!root.initialise(&error)) {
        if (parser.isSet(smokeOption)) {
            QTextStream(stderr) << "smoke: failed - " << error << Qt::endl;
            return 2;
        }
        QMessageBox::critical(nullptr, QCoreApplication::translate("cullfinch", "cullfinch"),
                              error);
        return 1;
    }

    cullfinch::ui::BrowserWindow* window = root.createBrowserWindow();

    // Dialogs belong to the composition root: the browser reports, the
    // application decides how loudly. Automated runs install neither and so
    // never block on a prompt nobody can dismiss.
    QObject::connect(window, &cullfinch::ui::BrowserWindow::errorOccurred, window,
                     [window](const QString& message) {
                         QMessageBox::warning(window,
                                              QCoreApplication::translate("cullfinch", "cullfinch"),
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

    if (parser.isSet(smokeOption)) {
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
