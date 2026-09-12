// SPDX-License-Identifier: GPL-3.0-or-later
#include "VisualBaseline.h"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QStringList>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTest>
#include <QWidget>

#include <algorithm>
#include <cstdlib>

namespace cullfinch::guitests {
namespace {

/// How long a rendering has to stay identical before it counts as settled.
/// Comfortably longer than the round trip from a resize to the decode it
/// triggers landing back on the GUI thread.
constexpr int kHoldMs = 200;

/// Everything masked out is filled with this, so a masked region is obvious in
/// a reference someone opens to look at.
constexpr QRgb kMaskColour = qRgb(0x20, 0x20, 0x20);

/// The pixel size every rendering uses. It cannot pin which font the machine
/// resolves, but it does remove the screen DPI from the metrics, which is one
/// fewer reason for two Linux boxes to disagree.
constexpr int kFontPixelSize = 13;

/// A string wide enough that two different fonts almost never advance it
/// identically. Part of the environment digest, never drawn.
QString metricsProbe() {
    return QStringLiteral("Cullfinch 0123456789 Read-only association issue(s)");
}

QImage renderOnce(QWidget* widget) {
    QImage image(widget->size(), QImage::Format_RGB32);
    image.setDevicePixelRatio(1.0);
    // A widget is entitled not to paint every pixel it owns; starting from a
    // stated colour keeps whatever it leaves alone reproducible.
    image.fill(Qt::white);
    widget->render(&image);
    return image;
}

QString resolveReferenceRoot() {
    return qEnvironmentVariable("CULLFINCH_VISUAL_REFERENCE_DIR",
                                QStringLiteral(CULLFINCH_VISUAL_REFERENCE_DIR));
}

QString resolveArtifactRoot() {
    // The same variable the session helpers publish their evidence through, so
    // a failing CI job uploads these alongside the compositor logs.
    QString root = qEnvironmentVariable("CULLFINCH_ARTIFACT_DIR");
    if (root.isEmpty()) {
        root = QCoreApplication::applicationDirPath();
    }
    return root + QStringLiteral("/visual");
}

/// Where two renderings disagree, and by how much.
struct PixelDifference {
    /// Magenta where the pixels differ, washed out where they do not.
    QImage map;
    qint64 differing = 0;
    int worst = 0;
};

PixelDifference comparePixels(const QImage& actual, const QImage& reference, int channelDelta) {
    PixelDifference result;
    result.map = QImage(actual.size(), QImage::Format_RGB32);

    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const QRgb rendered = actual.pixel(x, y);
            const QRgb expected = reference.pixel(x, y);
            const int delta = std::max({std::abs(qRed(rendered) - qRed(expected)),
                                        std::abs(qGreen(rendered) - qGreen(expected)),
                                        std::abs(qBlue(rendered) - qBlue(expected))});
            result.worst = std::max(result.worst, delta);

            const bool differs = delta > channelDelta;
            result.differing += differs ? 1 : 0;
            // Unchanged pixels stay visible, washed out, so the map reads as
            // "here, in this view" rather than as a scatter of dots on black.
            const int washed = (qGray(rendered) / 3) + 160;
            result.map.setPixel(x, y, differs ? qRgb(255, 0, 255) : qRgb(washed, washed, washed));
        }
    }
    return result;
}

} // namespace

bool pinSize(QWidget* widget, const QSize& size) {
    if (widget == nullptr || size.isEmpty()) {
        return false;
    }
    widget->setFixedSize(size);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return widget->size() == size;
}

QImage renderSettled(QWidget* widget, int timeoutMs) {
    if (widget == nullptr || widget->size().isEmpty()) {
        return {};
    }

    QElapsedTimer overall;
    overall.start();
    QElapsedTimer held;
    held.start();

    QImage previous;
    while (overall.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (QImage current = renderOnce(widget); previous.isNull() || current != previous) {
            previous = current;
            held.restart();
        } else if (held.elapsed() >= kHoldMs) {
            return current;
        }
        QTest::qWait(10);
    }
    return {};
}

void maskRegion(QImage& image, const QRect& region) {
    const QRect clipped = region.intersected(image.rect());
    if (clipped.isEmpty()) {
        return;
    }
    QPainter painter(&image);
    painter.fillRect(clipped, QColor(kMaskColour));
    painter.end();
}

void VisualBaseline::pinAppearance() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // A developer machine in dark mode hands every widget a different palette
    // than the one a reference was recorded against.
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif

    // Fusion is the one style available on every platform cullfinch builds
    // for, and its standard palette is stated by Qt rather than derived from
    // the desktop theme.
    if (QStyle* style = QStyleFactory::create(QStringLiteral("Fusion")); style != nullptr) {
        QApplication::setStyle(style);
        QApplication::setPalette(style->standardPalette());
    }

    QFont font = QApplication::font();
    font.setPixelSize(kFontPixelSize);
    QApplication::setFont(font);
}

VisualBaseline::Environment VisualBaseline::describeEnvironment() {
    const QFont font = QApplication::font();
    const QFontInfo resolved(font);
    const QFontMetrics metrics(font);
    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal ratio = (screen != nullptr) ? screen->devicePixelRatio() : 1.0;

    const QStringList inputs{
        QStringLiteral("qt = %1").arg(QString::fromLatin1(qVersion())),
        QStringLiteral("platform = %1").arg(QGuiApplication::platformName()),
        QStringLiteral("style = %1").arg(QApplication::style()->objectName()),
        QStringLiteral("devicePixelRatio = %1").arg(QString::number(ratio, 'f', 3)),
        QStringLiteral("font = %1 %2px").arg(resolved.family()).arg(resolved.pixelSize()),
        QStringLiteral("fontMetrics = %1/%2/%3")
            .arg(metrics.ascent())
            .arg(metrics.descent())
            .arg(metrics.horizontalAdvance(metricsProbe())),
    };

    Environment environment;
    environment.report = inputs.join(QLatin1Char('\n'));
    const QByteArray digest =
        QCryptographicHash::hash(environment.report.toUtf8(), QCryptographicHash::Sha256);
    environment.id = QStringLiteral("%1-%2-%3")
                         .arg(QGuiApplication::platformName(), QApplication::style()->objectName(),
                              QString::fromLatin1(digest.toHex().left(10)));
    return environment;
}

VisualBaseline::VisualBaseline()
    : referenceRoot_(resolveReferenceRoot()), artifactRoot_(resolveArtifactRoot()),
      environment_(describeEnvironment()),
      recording_(qEnvironmentVariableIsSet("CULLFINCH_UPDATE_VISUAL_REFERENCES")),
      requireReferences_(qEnvironmentVariableIsSet("CULLFINCH_REQUIRE_VISUAL_REFERENCES")) {}

QString VisualBaseline::referenceDirectory(VisualScope scope) const {
    return referenceRoot_ + QLatin1Char('/') +
           (scope == VisualScope::FontIndependent ? QStringLiteral("shared") : environment_.id);
}

bool VisualBaseline::write(const QString& path, const QImage& image, QString* error) const {
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("cannot create %1").arg(directory);
        return false;
    }
    if (!image.save(path, "PNG")) {
        *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

QString VisualBaseline::saveArtifact(const QString& path, const QImage& image) const {
    QString error;
    if (write(path, image, &error)) {
        return QStringLiteral("wrote %1").arg(path);
    }
    return QStringLiteral("could not write %1 (%2)").arg(path, error);
}

void VisualBaseline::writeEnvironmentReport() const {
    // Recorded next to the evidence either way: a reference that turns out to
    // have been taken somewhere unexpected is otherwise very hard to notice.
    if (!QDir().mkpath(artifactRoot_)) {
        return;
    }
    if (QFile file(artifactRoot_ + QStringLiteral("/environment.txt"));
        file.open(QIODevice::WriteOnly)) {
        const QString text =
            environment_.id + QLatin1Char('\n') + environment_.report + QLatin1Char('\n');
        file.write(text.toUtf8());
    }
}

VisualBaseline::Outcome VisualBaseline::record(const QString& caseName,
                                               const QString& referencePath, const QImage& actual,
                                               QString* report) const {
    QString error;
    if (!write(referencePath, actual, &error)) {
        *report = QStringLiteral("%1: %2").arg(caseName, error);
        return Outcome::Differed;
    }
    *report = QStringLiteral("%1: recorded %2").arg(caseName, referencePath);
    return Outcome::Recorded;
}

VisualBaseline::Outcome VisualBaseline::missing(const QString& caseName, const QString& directory,
                                                const QString& referencePath, const QImage& actual,
                                                QString* report) const {
    const QString candidate =
        artifactRoot_ + QLatin1Char('/') + caseName + QStringLiteral("-candidate.png");

    *report =
        QStringLiteral("%1: no reference at %2 for this rendering environment\n"
                       "%3\n"
                       "%4\n"
                       "record it with CULLFINCH_UPDATE_VISUAL_REFERENCES=1")
            .arg(caseName, referencePath, environment_.report, saveArtifact(candidate, actual));

    // A directory that exists but is missing this case is a suite somebody
    // extended without recording the new rendering: that is a failure. A
    // directory nobody has ever recorded is an environment this machine cannot
    // be held to, unless the job says otherwise.
    const bool known = QFileInfo::exists(directory);
    return (known || requireReferences_) ? Outcome::Differed : Outcome::Unrecorded;
}

VisualBaseline::Outcome VisualBaseline::verify(const QString& caseName,
                                               const QString& referencePath, const QImage& actual,
                                               const VisualTolerance& tolerance,
                                               QString* report) const {
    const QImage reference = QImage(referencePath).convertToFormat(QImage::Format_RGB32);
    const QString stem = artifactRoot_ + QLatin1Char('/') + caseName;

    if (reference.isNull()) {
        *report = QStringLiteral("%1: %2 is not a readable image").arg(caseName, referencePath);
        return Outcome::Differed;
    }

    if (reference.size() != actual.size()) {
        *report = QStringLiteral("%1: rendered %2x%3, reference is %4x%5\n%6")
                      .arg(caseName)
                      .arg(actual.width())
                      .arg(actual.height())
                      .arg(reference.width())
                      .arg(reference.height())
                      .arg(saveArtifact(stem + QStringLiteral("-actual.png"), actual));
        return Outcome::Differed;
    }

    const PixelDifference difference = comparePixels(actual, reference, tolerance.channelDelta);
    const auto total = static_cast<qint64>(actual.width()) * actual.height();
    const double fraction = static_cast<double>(difference.differing) / static_cast<double>(total);

    if (fraction <= tolerance.differingFraction) {
        *report = QStringLiteral("%1: matched (%2 of %3 pixels differ, worst %4)")
                      .arg(caseName)
                      .arg(difference.differing)
                      .arg(total)
                      .arg(difference.worst);
        return Outcome::Matched;
    }

    *report = QStringLiteral("%1: %2 of %3 pixels differ by more than %4 (worst %5)\n"
                             "%6\n%7\n%8")
                  .arg(caseName)
                  .arg(difference.differing)
                  .arg(total)
                  .arg(tolerance.channelDelta)
                  .arg(difference.worst)
                  .arg(saveArtifact(stem + QStringLiteral("-actual.png"), actual),
                       saveArtifact(stem + QStringLiteral("-expected.png"), reference),
                       saveArtifact(stem + QStringLiteral("-diff.png"), difference.map));
    return Outcome::Differed;
}

VisualBaseline::Outcome VisualBaseline::compare(const QString& caseName, const QImage& rendering,
                                                VisualScope scope, const VisualTolerance& tolerance,
                                                QString* message) const {
    QString discarded;
    QString* report = (message != nullptr) ? message : &discarded;

    const QString directory = referenceDirectory(scope);
    const QString referencePath = directory + QLatin1Char('/') + caseName + QStringLiteral(".png");
    const QImage actual = rendering.convertToFormat(QImage::Format_RGB32);

    writeEnvironmentReport();

    if (actual.isNull()) {
        *report = QStringLiteral("%1: nothing was rendered").arg(caseName);
        return Outcome::Differed;
    }
    if (recording_) {
        return record(caseName, referencePath, actual, report);
    }
    if (!QFileInfo::exists(referencePath)) {
        return missing(caseName, directory, referencePath, actual, report);
    }
    return verify(caseName, referencePath, actual, tolerance, report);
}

} // namespace cullfinch::guitests
