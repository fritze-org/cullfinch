// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/ui/AssetPresentation.h>

#include <QList>
#include <QString>
#include <QStringList>

namespace cullfinch::cropper {

/// One step of the sequence: a photo, and the files a crop of it is written
/// beside.
struct CropTarget {
    ui::AssetPresentation presentation;
    QString jpegPath;
    QStringList rawPaths;
};

/// The photos to crop in one directory, in the order to crop them.
struct CropPlan {
    QList<CropTarget> targets;
    /// RAW files on their own. Nothing here can draw them, so they are
    /// counted rather than shown -- the window says they exist.
    int withoutPreview = 0;
    /// Photos left out because an earlier run, or another program, already
    /// cropped them.
    int alreadyCropped = 0;
    /// Whether subdirectories were entered.
    bool recursive = false;
    /// Why the directory could not be read. Empty on success, including for a
    /// directory that holds nothing to crop.
    QString error;
};

/// True when `target` already has a crop: its `.crop` JPG exists, or a RAW's
/// sidecar records one.
[[nodiscard]] bool isAlreadyCropped(const CropTarget& target);

/// The photos in `directory` to crop, grouped and ordered exactly as the
/// browser and the wall show them, entering subdirectories only when
/// `recursive` is set.
///
/// This tool's own output never becomes a step: `IMG_1.crop.jpg` is a photo to
/// every other part of Cullfinch, but cropping a crop is never what a second
/// run is for. Photos already cropped are left out unless `redo` is set, so a
/// run that was stopped resumes where it stopped.
[[nodiscard]] CropPlan planCrops(const QString& directory, bool recursive, bool redo);

} // namespace cullfinch::cropper
