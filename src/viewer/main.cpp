// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <cullfinch/infrastructure/QtImageService.h>
#include <cullfinch/viewer/DirectoryPhotos.h>
#include <cullfinch/viewer/WallViewerWindow.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>
#include <QTextStream>

#include <span>

namespace {

/// What the image cache and in-flight decodes may use together. The same
/// budget the full application gives its comparison views.
constexpr qint64 kImageMemoryBudgetBytes = 512LL * 1024 * 1024;

/// Above this many photos the wall starts as a scrolling grid rather than
/// fitted: fitting a few hundred photos onto one screen makes every tile too
/// small to see anything in, and decodes every one of them at once.
constexpr int kFitAtMost = 48;
constexpr int kDefaultTileWidth = 256;

QString tileWidthKey() {
    return QStringLiteral("wall/tileWidth");
}

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch-wall", text);
}

/// Non-interactive verification of the *deployed* package: a real JPEG from
/// `directory` decodes through the deployed image plugin in a window on the
/// deployed platform plugin. A smoke path, not a product feature.
int runSmoke(const cullfinch::viewer::DirectoryPhotos& photos,
             const cullfinch::infrastructure::QtImageService& images) {
    QTextStream out(stdout);
    if (photos.photos.isEmpty()) {
        out << "smoke: failed - the directory holds no photos" << Qt::endl;
        return 2;
    }

    QElapsedTimer timer;
    timer.start();
    while (images.memoryUsedBytes() == 0 && timer.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    if (images.memoryUsedBytes() == 0) {
        out << "smoke: failed - no image decoded" << Qt::endl;
        return 2;
    }

    // Named for the same reason the full application's smoke run names it: a
    // package that only works through XWayland looks identical otherwise.
    out << "smoke: platform=" << QGuiApplication::platformName() << Qt::endl;
    out << "smoke: ok" << Qt::endl;
    return 0;
}

struct CommandLine {
    QCommandLineParser parser;
    QCommandLineOption smoke{QStringLiteral("smoke"),
                             tr("Run a non-interactive package verification and exit.")};

    CommandLine() {
        parser.setApplicationDescription(
            tr("Show every photo in a directory on one wall. Nothing is marked, moved or "
               "deleted."));
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addPositionalArgument(QStringLiteral("directory"),
                                     tr("Directory of photos to show. Asked for when omitted."));
        parser.addOption(smoke);
    }
};

/// True when the arguments only ask something that needs no display.
bool wantsConsoleOnly(std::span<char* const> arguments) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const QString argument = QString::fromLocal8Bit(arguments[index]);
        if (argument == QLatin1String("--version") || argument == QLatin1String("-v") ||
            argument == QLatin1String("--help") || argument == QLatin1String("-h")) {
            return true;
        }
    }
    return false;
}

void setApplicationIdentity() {
    // A name of its own, so the viewer's settings never mix with the full
    // application's.
    QCoreApplication::setOrganizationName(QStringLiteral("cullfinch"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("cullfinch.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("cullfinch-wall"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CULLFINCH_VERSION));
}

} // namespace

int main(int argc, char* argv[]) {
    // --version and --help must answer without a display, as they do for the
    // full application.
    if (wantsConsoleOnly({argv, static_cast<std::size_t>(argc)})) {
        QCoreApplication console(argc, argv);
        setApplicationIdentity();
        CommandLine commandLine;
        commandLine.parser.process(console);
        return 0;
    }

    cullfinch::app::preferPortalDialogs();
    QApplication application(argc, argv);
    setApplicationIdentity();
    cullfinch::app::pruneUnreachableIconThemePaths();
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Cullfinch Wall"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/org.fritze.cullfinch.png")));
    // Matches the installed .desktop basename; see the full application.
    QGuiApplication::setDesktopFileName(QStringLiteral(CULLFINCH_DESKTOP_ID));

    CommandLine commandLine;
    commandLine.parser.process(application);
    const bool smoke = commandLine.parser.isSet(commandLine.smoke);

    cullfinch::app::reportPlatformBackend(QStringLiteral("cullfinch-wall"),
                                          QStringLiteral(CULLFINCH_VERSION));

    QString directory;
    if (const QStringList arguments = commandLine.parser.positionalArguments();
        !arguments.isEmpty()) {
        directory = QDir(arguments.first()).absolutePath();
    } else if (smoke) {
        QTextStream(stdout) << "smoke: failed - no directory argument" << Qt::endl;
        return 2;
    } else {
        directory = QFileDialog::getExistingDirectory(nullptr, tr("Show photos from"));
        if (directory.isEmpty()) {
            return 0; // Cancelled: nothing was asked of us.
        }
    }

    const cullfinch::viewer::DirectoryPhotos photos =
        cullfinch::viewer::loadDirectoryPhotos(directory);
    if (!photos.error.isEmpty()) {
        if (smoke) {
            QTextStream(stdout) << "smoke: failed - " << photos.error << Qt::endl;
            return 2;
        }
        QMessageBox::critical(nullptr, QGuiApplication::applicationDisplayName(), photos.error);
        return 1;
    }

    cullfinch::infrastructure::QtImageService images;
    images.setMemoryBudgetBytes(kImageMemoryBudgetBytes);

    // A smoke run never reads or writes the settings: it runs against a clean
    // environment and must not leave anything in it.
    QSettings settings;
    const int suggested = photos.photos.size() > kFitAtMost ? kDefaultTileWidth : 0;
    const int tileWidth = smoke ? 0 : settings.value(tileWidthKey(), suggested).toInt();

    cullfinch::viewer::WallViewerWindow window(images, directory, photos, tileWidth);
    if (!smoke) {
        QObject::connect(&window, &cullfinch::viewer::WallViewerWindow::tileWidthChanged, &window,
                         [&settings](int pixels) { settings.setValue(tileWidthKey(), pixels); });
    }
    window.resize(1280, 800);
    window.show();

    if (smoke) {
        return runSmoke(photos, images);
    }
    return QApplication::exec();
}
