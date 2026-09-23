// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/ui/AssetPresentation.h>

#include <QList>
#include <QString>

namespace cullfinch::viewer {

/// The photos in one directory, as the standalone wall shows them.
struct DirectoryPhotos {
    /// One entry per photo -- a JPG and its RAW companions together -- in
    /// natural filename order.
    QList<ui::AssetPresentation> photos;
    /// Groups left out because nothing in them can be drawn: RAW files on their
    /// own. Counted so the window can say they exist rather than hide them.
    int withoutPreview = 0;
    /// Why the directory could not be read. Empty on success, including for a
    /// directory that simply holds no photos.
    QString error;
};

/// Read `directory`, without entering subdirectories, and group its files
/// exactly as the browser does.
///
/// Reuses the browser's own scanner and pairing policy instead of listing
/// `*.jpg`, so a JPG and its RAW are one tile here as well. Nothing is
/// written: no collection is recorded, no database is opened.
[[nodiscard]] DirectoryPhotos loadDirectoryPhotos(const QString& directory);

} // namespace cullfinch::viewer
