// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRectF>
#include <QString>
#include <QStringList>

namespace cullfinch::cropper {

/// One accepted crop: what to write it from, and where on the photo.
struct CropJob {
    /// The photo's JPG. The crop is written next to it.
    QString jpegPath;
    /// The photo's RAW companions. Each gets the same crop in its sidecar.
    QStringList rawPaths;
    /// Normalised, in the photo as displayed -- after its EXIF orientation.
    QRectF rect;
};

/// Where the crop of `jpegPath` is written: `IMG_1234.crop.jpg` beside
/// `IMG_1234.jpg`, keeping the original's extension as it is spelt.
///
/// A sibling rather than a replacement, so the original is never touched, and
/// a name the next run can recognise as a crop and leave out of the sequence.
[[nodiscard]] QString cropOutputPath(const QString& jpegPath);

/// True for a file stem this tool produced, such as `IMG_1234.crop`.
[[nodiscard]] bool isCropOutputStem(const QString& stem);

/// Write `rect` of the JPG at `sourcePath` to `cropOutputPath(sourcePath)`.
///
/// The image is decoded upright, cropped and encoded at quality 95. Its
/// metadata -- EXIF, IPTC, XMP and the colour profile -- is copied from the
/// original, with the orientation reset to normal because the pixels are now
/// stored the right way up, the pixel dimensions set to the crop's, and the
/// embedded thumbnail dropped because it would still show the whole photo.
///
/// The file is assembled under a temporary name in the same directory and
/// renamed into place, so an interrupted write leaves no half-written crop
/// for the next run to mistake for a finished one.
///
/// @return an error message, or an empty string on success.
[[nodiscard]] QString writeJpegCrop(const QString& sourcePath, const QRectF& rect);

/// Write everything `job` asks for: the JPG crop and each RAW's sidecar.
///
/// Safe to call from a worker thread.
///
/// @return the errors, one per line, or an empty string on success.
[[nodiscard]] QString saveCrop(const CropJob& job);

/// Prepare the metadata library before any thread uses it. Its XMP parser
/// must be initialised once, and not concurrently; everything here calls this
/// first, so it only needs calling explicitly to be certain of the thread.
void initialiseMetadataLibrary();

} // namespace cullfinch::cropper
