// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QRectF>
#include <QString>

namespace cullfinch::cropper {

/// The Camera Raw sidecar of `rawPath`: `IMG_1234.xmp` beside `IMG_1234.CR2`.
///
/// Adobe's naming, which drops the RAW's extension; Lightroom and Camera Raw
/// read the crop from it. darktable keeps its own sidecar under
/// `IMG_1234.CR2.xmp` and rewrites it from its library, so a crop written there
/// would be ignored and then lost. Its Lightroom import does find this file,
/// but accepts only one whose history says Adobe software wrote it, which
/// this one truthfully does not. See decision 0012.
[[nodiscard]] QString sidecarPath(const QString& rawPath);

/// True when the sidecar of `rawPath` already records a crop.
[[nodiscard]] bool hasRecordedCrop(const QString& rawPath);

/// Record `storedRect` as the crop of `rawPath` in its sidecar.
///
/// `storedRect` is normalised in the frame the RAW's pixels are stored in --
/// before its orientation is applied -- which is the frame Camera Raw's
/// `crs:Crop*` fields are measured in. An existing sidecar is merged into:
/// only the crop fields change. One that cannot be read is left alone and
/// reported rather than replaced, because it may hold somebody's edits.
///
/// @return an error message, or an empty string on success.
[[nodiscard]] QString writeCameraRawCrop(const QString& rawPath, const QRectF& storedRect);

/// The EXIF orientation (1-8) recorded in the file at `path`, or 0 when it
/// cannot be read or records none.
[[nodiscard]] int storedOrientation(const QString& path);

} // namespace cullfinch::cropper
