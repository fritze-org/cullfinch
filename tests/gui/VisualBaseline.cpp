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
#include <tuple>

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
        QImage current = renderOnce(widget);
        if (previous.isNull() || current != previous) {
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
    QStyle* style = QStyleFactory::create(QStringLiteral("Fusion"));
    if (style != nullptr) {
        QApplication::setStyle(style);
        QApplication::setPalette(style->standardPalette());
    }

    QFont font = QApplication::font();
    font.setPixelSize(kFontPixelSize);
    QApplication::setFont(font);
}

VisualBaseline::VisualBaseline() {
    referenceRoot_ = qEnvironmentVariable("CULLFINCH_VISUAL_REFERENCE_DIR",
                                          QStringLiteral(CULLFINCH_VISUAL_REFERENCE_DIR));

    // The same variable the session helpers publish their evidence through, so
    // a failing CI job uploads these alongside the compositor logs.
    artifactRoot_ = qEnvironmentVariable("CULLFINCH_ARTIFACT_DIR");
    if (artifactRoot_.isEmpty()) {
        artifactRoot_ = QCoreApplication::applicationDirPath();
    }
    artifactRoot_ += QStringLiteral("/visual");

    recording_ = qEnvironmentVariableIsSet("CULLFINCH_UPDATE_VISUAL_REFERENCES");
    requireReferences_ = qEnvironmentVariableIsSet("CULLFINCH_REQUIRE_VISUAL_REFERENCES");

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
    environmentReport_ = inputs.join(QLatin1Char('\n'));

    const QByteArray digest =
        QCryptographicHash::hash(environmentReport_.toUtf8(), QCryptographicHash::Sha256);
    environmentId_ = QStringLiteral("%1-%2-%3")
                         .arg(QGuiApplication::platformName(), QApplication::style()->objectName(),
                              QString::fromLatin1(digest.toHex().left(10)));
}

QString VisualBaseline::referenceDirectory(VisualScope scope) const {
    return referenceRoot_ + QLatin1Char('/') +
           (scope == VisualScope::FontIndependent ? QStringLiteral("shared") : environmentId_);
}

bool VisualBaseline::write(const QString& path, const QImage& image, QString* error) const {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    if (!image.save(path, "PNG")) {
        *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

VisualBaseline::Outcome VisualBaseline::compare(const QString& caseName, const QImage& rendering,
                                                VisualScope scope, const VisualTolerance& tolerance,
                                                QString* message) {
    QString report;
    Outcome outcome = Outcome::Differed;

    const QString directory = referenceDirectory(scope);
    const QString referencePath = directory + QLatin1Char('/') + caseName + QStringLiteral(".png");
    const QImage actual = rendering.convertToFormat(QImage::Format_RGB32);

    // The environment is recorded next to the evidence either way: a reference
    // that turns out to have been taken somewhere unexpected is otherwise very
    // hard to notice.
    QFile environmentFile(artifactRoot_ + QStringLiteral("/environment.txt"));
    if (QDir().mkpath(artifactRoot_) && environmentFile.open(QIODevice::WriteOnly)) {
        environmentFile.write(
            (environmentId_ + QLatin1Char('\n') + environmentReport_ + QLatin1Char('\n')).toUtf8());
        environmentFile.close();
    }

    if (actual.isNull()) {
        report = QStringLiteral("%1: nothing was rendered").arg(caseName);
    } else if (recording_) {
        QString error;
        if (write(referencePath, actual, &error)) {
            outcome = Outcome::Recorded;
            report = QStringLiteral("%1: recorded %2").arg(caseName, referencePath);
        } else {
            report = QStringLiteral("%1: %2").arg(caseName, error);
        }
    } else if (!QFileInfo::exists(referencePath)) {
        QString error;
        const QString candidate =
            artifactRoot_ + QLatin1Char('/') + caseName + QStringLiteral("-candidate.png");
        std::ignore = write(candidate, actual, &error);

        // A directory that exists but is missing this case is a suite someone
        // extended without recording the new rendering: that is a failure. A
        // directory nobody has ever recorded is an environment this machine
        // simply cannot be held to, unless the job says otherwise.
        const bool known = QFileInfo::exists(directory);
        outcome = (known || requireReferences_) ? Outcome::Differed : Outcome::Unrecorded;
        report = QStringLiteral("%1: no reference at %2 for this rendering environment\n"
                                "%3\n"
                                "candidate written to %4\n"
                                "record it with CULLFINCH_UPDATE_VISUAL_REFERENCES=1")
                     .arg(caseName, referencePath, environmentReport_, candidate);
    } else {
        QImage reference(referencePath);
        reference = reference.convertToFormat(QImage::Format_RGB32);
        if (reference.isNull()) {
            report = QStringLiteral("%1: %2 is not a readable image").arg(caseName, referencePath);
        } else if (reference.size() != actual.size()) {
            QString error;
            std::ignore =
                write(artifactRoot_ + QLatin1Char('/') + caseName + QStringLiteral("-actual.png"),
                      actual, &error);
            report = QStringLiteral("%1: rendered %2x%3, reference is %4x%5")
                         .arg(caseName)
                         .arg(actual.width())
                         .arg(actual.height())
                         .arg(reference.width())
                         .arg(reference.height());
        } else {
            QImage difference(actual.size(), QImage::Format_RGB32);
            qint64 differing = 0;
            int worst = 0;

            for (int y = 0; y < actual.height(); ++y) {
                const auto* actualRow = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
                const auto* referenceRow =
                    reinterpret_cast<const QRgb*>(reference.constScanLine(y));
                auto* differenceRow = reinterpret_cast<QRgb*>(difference.scanLine(y));
                for (int x = 0; x < actual.width(); ++x) {
                    const int delta =
                        std::max({std::abs(qRed(actualRow[x]) - qRed(referenceRow[x])),
                                  std::abs(qGreen(actualRow[x]) - qGreen(referenceRow[x])),
                                  std::abs(qBlue(actualRow[x]) - qBlue(referenceRow[x]))});
                    worst = std::max(worst, delta);
                    if (delta > tolerance.channelDelta) {
                        ++differing;
                        differenceRow[x] = qRgb(255, 0, 255);
                    } else {
                        // The unchanged pixels stay visible, washed out, so a
                        // diff image reads as "here, in this view" rather than
                        // as a scatter of dots on black.
                        const int washed = (qGray(actualRow[x]) / 3) + 160;
                        differenceRow[x] = qRgb(washed, washed, washed);
                    }
                }
            }

            const auto total = static_cast<qint64>(actual.width()) * actual.height();
            const double fraction = static_cast<double>(differing) / static_cast<double>(total);
            if (fraction > tolerance.differingFraction) {
                QString error;
                const QString stem = artifactRoot_ + QLatin1Char('/') + caseName;
                std::ignore = write(stem + QStringLiteral("-actual.png"), actual, &error);
                std::ignore = write(stem + QStringLiteral("-expected.png"), reference, &error);
                std::ignore = write(stem + QStringLiteral("-diff.png"), difference, &error);
                report = QStringLiteral("%1: %2 of %3 pixels differ by more than %4 "
                                        "(worst %5); images written to %6-*.png")
                             .arg(caseName)
                             .arg(differing)
                             .arg(total)
                             .arg(tolerance.channelDelta)
                             .arg(worst)
                             .arg(stem);
            } else {
                outcome = Outcome::Matched;
                report = QStringLiteral("%1: matched (%2 of %3 pixels differ, worst %4)")
                             .arg(caseName)
                             .arg(differing)
                             .arg(total)
                             .arg(worst);
            }
        }
    }

    if (message != nullptr) {
        *message = report;
    }
    return outcome;
}

} // namespace cullfinch::guitests
