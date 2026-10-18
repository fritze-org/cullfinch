// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/ui/AssetPresentation.h>

#include <QList>
#include <QString>

namespace cullfinch::viewer {

/// The photos in one directory, as the standalone wall shows them.
struct DirectoryPhotos {
    /// One entry per photo -- a JPG and its RAW companions together -- in
    /// natural filename order, by directory first when subdirectories were
    /// included.
    QList<ui::AssetPresentation> photos;
    /// Groups left out because nothing in them can be drawn: RAW files on their
    /// own. Counted so the window can say they exist rather than hide them.
    int withoutPreview = 0;
    /// Whether subdirectories were entered. Recorded because the window says so
    /// -- a count that covers a whole tree is a different claim from one that
    /// covers a folder.
    bool recursive = false;
    /// Why the directory could not be read. Empty on success, including for a
    /// directory that simply holds no photos.
    QString error;
};

/// Read `directory` and group its files exactly as the browser does, entering
/// subdirectories only when `recursive` is set.
///
/// Reuses the browser's own scanner and pairing policy instead of listing
/// `*.jpg`, so a JPG and its RAW are one tile here as well. Pairing stays
/// within a directory, so the same filename stem in two subdirectories is two
/// photos, not one photo with a companion. Nothing is written: no collection is
/// recorded, no database is opened.
[[nodiscard]] DirectoryPhotos loadDirectoryPhotos(const QString& directory, bool recursive = false);

} // namespace cullfinch::viewer
