// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

class QWidget;

namespace cullfinch::guitests {

/// How far two renderings of the same view may drift and still be one picture.
///
/// Not zero. JPEG decoding, smooth scaling and the painter's own rounding are
/// bit-exact on one machine and only nearly so across Qt builds, so a handful
/// of pixels off by a level or two is noise. A widget that moved is thousands
/// of pixels off by a lot, which is the whole point of comparing at all.
struct VisualTolerance {
    /// Largest per-channel difference a pixel may show and still count as
    /// unchanged.
    int channelDelta = 6;
    /// Fraction of the image allowed to exceed that before the case fails.
    double differingFraction = 0.002;
};

/// Whether a rendering's pixels depend on the fonts the machine happens to
/// have installed.
///
/// Glyph rasterisation is not reproducible across font stacks, so a rendering
/// that contains text can only be compared against a reference recorded in the
/// same environment. Renderings that draw no text -- or whose text has been
/// masked out -- are compared against one shared reference everywhere.
enum class VisualScope {
    FontIndependent,
    FontDependent,
};

/// Pin a widget to an exact size, whatever its surrounding layout would give
/// it, so a reference records the view and not the machine's font metrics.
///
/// The constraint is deliberately not lifted afterwards: a test that has
/// rendered a widget is finished measuring it.
[[nodiscard]] bool pinSize(QWidget* widget, const QSize& size);

/// Render a widget offscreen, once it has stopped changing.
///
/// Pinning a size resizes the previews inside the view, and an ImageCanvas
/// answers a resize by requesting a decode at the new size -- so the first
/// rendering after a resize is the old image stretched, and the one after the
/// decode lands is the real thing. Neither a signal nor a readiness flag
/// separates them: readiness stays true across the refinement. What the test
/// actually needs is that the picture has stopped moving, so that is what is
/// waited on, the same way settleWindowSize() waits for a size to hold.
///
/// @return a null image if the rendering never held still, which the caller
///         should treat as a failure rather than compare.
[[nodiscard]] QImage renderSettled(QWidget* widget, int timeoutMs = 20000);

/// Flatten a rectangle whose content is glyphs.
///
/// Masking a *fixed* region -- a caption strip pinned to the bottom of a tile,
/// the label band of a fixed-size grid cell -- keeps the comparison honest:
/// the geometry that decides where the region is stays compared, only the
/// glyph rasterisation inside it is dropped.
void maskRegion(QImage& image, const QRect& region);

/// Compares a rendering against a checked-in reference, and records the
/// evidence when it does not match.
///
/// References live under `tests/fixtures/visual/`, in `shared/` for renderings
/// that draw no text and in a per-environment directory otherwise. The
/// environment is identified by a digest of everything the rendering depends
/// on that is not cullfinch: the Qt version, the platform plugin, the widget
/// style, the device pixel ratio and the resolved default font's metrics.
///
/// Environment variables:
///
/// - `CULLFINCH_UPDATE_VISUAL_REFERENCES` records every case into the source
///   tree instead of comparing. Review the diff; that is the whole approval
///   step.
/// - `CULLFINCH_REQUIRE_VISUAL_REFERENCES` turns "no reference exists for this
///   environment" from a skip into a failure, for a job that is meant to be
///   gating rather than advisory.
/// - `CULLFINCH_ARTIFACT_DIR` receives the actual, expected and difference
///   images of every case that failed, and the candidate reference of every
///   case that could not be compared.
class VisualBaseline {
public:
    VisualBaseline();

    /// Pin every appearance input that is not the code under test: the widget
    /// style, its palette and the colour scheme. Call once, before any widget
    /// is built.
    static void pinAppearance();

    enum class Outcome {
        Matched,    ///< Within tolerance of the recorded reference.
        Recorded,   ///< Written to the source tree, because recording was asked for.
        Unrecorded, ///< No reference exists for this environment yet.
        Differed,   ///< A real difference, or a reference that cannot be read.
    };

    /// @param message receives a report in every case, including a match.
    Outcome compare(const QString& caseName, const QImage& rendering, VisualScope scope,
                    const VisualTolerance& tolerance, QString* message) const;

    /// Directory name identifying this rendering environment.
    [[nodiscard]] QString environmentId() const { return environment_.id; }
    /// The inputs that identifier was derived from, one per line.
    [[nodiscard]] QString environmentReport() const { return environment_.report; }

private:
    /// Everything a rendering depends on that is not cullfinch, as a directory
    /// name and as the inputs behind it. Held as one value so the two cannot
    /// be built from different states.
    struct Environment {
        QString id;
        QString report;
    };
    [[nodiscard]] static Environment describeEnvironment();

    [[nodiscard]] QString referenceDirectory(VisualScope scope) const;
    [[nodiscard]] bool write(const QString& path, const QImage& image, QString* error) const;
    void writeEnvironmentReport() const;

    [[nodiscard]] Outcome record(const QString& caseName, const QString& referencePath,
                                 const QImage& actual, QString* report) const;
    [[nodiscard]] Outcome missing(const QString& caseName, const QString& directory,
                                  const QString& referencePath, const QImage& actual,
                                  QString* report) const;
    [[nodiscard]] Outcome verify(const QString& caseName, const QString& referencePath,
                                 const QImage& actual, const VisualTolerance& tolerance,
                                 QString* report) const;

    QString referenceRoot_;
    QString artifactRoot_;
    Environment environment_;
    bool recording_ = false;
    bool requireReferences_ = false;
};

} // namespace cullfinch::guitests
