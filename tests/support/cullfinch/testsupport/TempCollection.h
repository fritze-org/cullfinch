// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QString>
#include <QTemporaryDir>

#include <memory>

namespace cullfinch::testsupport {

/// A disposable directory of photos.
///
/// JPEGs are generated at run time rather than committed as binaries: it keeps
/// the repository free of opaque files, exercises the deployed JPEG plugin on
/// every run, and lets case-colliding filenames be created only on volumes that
/// can actually hold them.
class TempCollection {
public:
    TempCollection();

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString path() const;
    [[nodiscard]] QString filePath(const QString& relative) const;

    /// Write a real JPEG of the given size, filled with a distinguishable
    /// colour derived from the name.
    QString addJpeg(const QString& relative, const QSize& size = QSize(160, 120));

    /// Write an opaque companion. cullfinch never decodes these bytes, so any
    /// content is fine; tests assert the bytes survive unchanged.
    QString addRaw(const QString& relative,
                   const QByteArray& contents = QByteArrayLiteral("RAWDATA"));

    /// Write an arbitrary file, for unclassified same-stem and sidecar cases.
    QString addFile(const QString& relative, const QByteArray& contents);

    /// A JPEG whose bytes are truncated, so decoding fails but the file exists.
    QString addCorruptJpeg(const QString& relative);

    /// Create a subdirectory.
    bool addDirectory(const QString& relative);

    /// @return false when the filesystem cannot hold both names, which is a
    ///         legitimate skip rather than a failure.
    [[nodiscard]] bool supportsCaseDistinctNames() const;

private:
    std::unique_ptr<QTemporaryDir> directory_;
};

} // namespace cullfinch::testsupport
