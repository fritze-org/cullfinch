// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/viewer/DirectoryPhotos.h>

#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <QDir>

namespace cullfinch::viewer {

DirectoryPhotos loadDirectoryPhotos(const QString& directory, bool recursive) {
    DirectoryPhotos result;
    result.recursive = recursive;
    const QString root = QDir(directory).absolutePath();

    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(root, recursive, &result.error);
    if (!result.error.isEmpty()) {
        return result;
    }

    // The collection identity only seeds the asset identifiers, which live
    // exactly as long as this window; nothing is looked up by it.
    const domain::CollectionId collection(root);
    const domain::AssociationResult grouped = domain::StemAssociationResolver{}.resolve(
        collection, files, domain::AssociationConfig::defaults(), true);

    // The resolver returns its assets ordered by relative directory and then by
    // natural stem, which is the order to show them in: a subdirectory's photos
    // belong together. Re-sorting on the display name would interleave trees,
    // because the name a caption shows is a path once subdirectories are in.
    for (const domain::PhotoAsset& asset : grouped.assets) {
        // Anything with a preview is shown, ambiguous pairings and symlinks
        // included: those make a photo unsafe to move, not unsafe to look at.
        if (asset.preview() == nullptr) {
            ++result.withoutPreview;
            continue;
        }
        result.photos.append(ui::AssetPresentation::from(asset));
    }
    return result;
}

} // namespace cullfinch::viewer
