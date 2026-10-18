// SPDX-License-Identifier: GPL-3.0-or-later
#include <DesktopIntegration.h>

#include <cullfinch/cropper/CropSequence.h>
#include <cullfinch/cropper/CropWindow.h>
#include <cullfinch/cropper/CropWriter.h>
#include <cullfinch/infrastructure/QtImageService.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QIcon>
#include <QMessageBox>
#include <QTextStream>

#include <span>

namespace {

/// What the image cache and in-flight decodes may use together. The same
/// budget the wall viewer and the comparison views have.
constexpr qint64 kImageMemoryBudgetBytes = 512LL * 1024 * 1024;

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch-crop", text);
}

QString trn(const char* text, int count) {
    return QCoreApplication::translate("cullfinch-crop", text, nullptr, count);
}

/// Non-interactive verification of the *deployed* package: a real JPEG from
/// the directory decodes through the deployed image plugin in a window on the
/// deployed platform plugin, and the metadata library loads. Nothing is
/// written. A smoke path, not a product feature.
int runSmoke(const cullfinch::cropper::CropWindow& window) {
    QTextStream out(stdout);
    if (window.photoCount() == 0) {
        out << "smoke: failed - the directory holds nothing to crop" << Qt::endl;
        return 2;
    }
    QElapsedTimer timer;
    timer.start();
    while (!window.canvas()->isReady() && timer.elapsed() < 30000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    if (!window.canvas()->isReady()) {
        out << "smoke: failed - no image decoded" << Qt::endl;
        return 2;
    }
    out << "smoke: platform=" << QGuiApplication::platformName() << Qt::endl;
    out << "smoke: ok" << Qt::endl;
    return 0;
}

struct CommandLine {
    QCommandLineParser parser;
    QCommandLineOption recursive{{QStringLiteral("r"), QStringLiteral("recursive")},
                                 tr("Also crop photos in subdirectories.")};
    QCommandLineOption redo{QStringLiteral("redo"),
                            tr("Also show photos that already have a crop; saving replaces it.")};
    QCommandLineOption smoke{QStringLiteral("smoke"),
                             tr("Run a non-interactive package verification and exit.")};

    CommandLine() {
        parser.setApplicationDescription(
            tr("Crop every photo in a directory, one after another. Each crop is written next "
               "to its photo as NAME.crop.jpg, and into NAME.xmp for a RAW companion; the "
               "originals are never changed."));
        parser.addHelpOption();
        parser.addVersionOption();
        parser.addPositionalArgument(QStringLiteral("directory"),
                                     tr("Directory of photos to crop. Asked for when omitted."));
        parser.addOption(recursive);
        parser.addOption(redo);
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
    QCoreApplication::setOrganizationName(QStringLiteral("cullfinch"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("cullfinch.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("cullfinch-crop"));
    QCoreApplication::setApplicationVersion(QStringLiteral(CULLFINCH_VERSION));
}

} // namespace

int main(int argc, char* argv[]) {
    if (wantsConsoleOnly({argv, static_cast<std::size_t>(argc)})) {
        QCoreApplication console(argc, argv);
        setApplicationIdentity();
        CommandLine commandLine;
        commandLine.parser.process(console);
        return 0;
    }

    const cullfinch::app::DesktopStartup desktop({argv, static_cast<std::size_t>(argc)});
    QApplication application(argc, argv);
    setApplicationIdentity();
    cullfinch::app::pruneUnreachableIconThemePaths();
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Cullfinch Crop"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/org.fritze.cullfinch.png")));
    QGuiApplication::setDesktopFileName(QStringLiteral(CULLFINCH_DESKTOP_ID));

    CommandLine commandLine;
    commandLine.parser.process(application);
    const bool smoke = commandLine.parser.isSet(commandLine.smoke);
    // Neither is remembered: both decide which photos a run covers, and a run
    // should cover what it was asked to.
    const bool recursive = commandLine.parser.isSet(commandLine.recursive);
    const bool redo = commandLine.parser.isSet(commandLine.redo);

    desktop.reportBackend(QStringLiteral("cullfinch-crop"), QStringLiteral(CULLFINCH_VERSION));
    // Before any write can start on the worker thread.
    cullfinch::cropper::initialiseMetadataLibrary();

    QString directory;
    if (const QStringList arguments = commandLine.parser.positionalArguments();
        !arguments.isEmpty()) {
        directory = QDir(arguments.first()).absolutePath();
    } else if (smoke) {
        QTextStream(stdout) << "smoke: failed - no directory argument" << Qt::endl;
        return 2;
    } else {
        directory = QFileDialog::getExistingDirectory(nullptr, tr("Crop photos from"));
        if (directory.isEmpty()) {
            return 0; // Cancelled: nothing was asked of us.
        }
    }

    // A smoke run looks at every photo, cropped or not: it verifies decoding,
    // and a package test directory may well hold an earlier crop.
    const cullfinch::cropper::CropPlan plan =
        cullfinch::cropper::planCrops(directory, recursive, redo || smoke);
    if (!plan.error.isEmpty()) {
        if (smoke) {
            QTextStream(stdout) << "smoke: failed - " << plan.error << Qt::endl;
            return 2;
        }
        QMessageBox::critical(nullptr, QGuiApplication::applicationDisplayName(), plan.error);
        return 1;
    }
    if (plan.targets.isEmpty() && !smoke) {
        QString text =
            tr("There is nothing to crop in %1.").arg(QDir::toNativeSeparators(directory));
        if (plan.alreadyCropped > 0) {
            text +=
                QLatin1Char('\n') + trn("%n photo(s) already have a crop; --redo shows them again.",
                                        plan.alreadyCropped);
        }
        QMessageBox::information(nullptr, QGuiApplication::applicationDisplayName(), text);
        return 0;
    }

    cullfinch::infrastructure::QtImageService images;
    images.setMemoryBudgetBytes(kImageMemoryBudgetBytes);

    cullfinch::cropper::CropWindow window(images, plan);
    window.setWindowTitle(QDir::toNativeSeparators(directory));
    window.resize(1280, 860);
    window.show();

    if (smoke) {
        return runSmoke(window);
    }
    return QApplication::exec();
}
