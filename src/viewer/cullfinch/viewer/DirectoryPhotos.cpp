// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/viewer/DirectoryPhotos.h>

#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/domain/NaturalOrder.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <QDir>

#include <algorithm>

namespace cullfinch::viewer {

DirectoryPhotos loadDirectoryPhotos(const QString& directory) {
    DirectoryPhotos result;
    const QString root = QDir(directory).absolutePath();

    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(root, false, &result.error);
    if (!result.error.isEmpty()) {
        return result;
    }

    // The collection identity only seeds the asset identifiers, which live
    // exactly as long as this window; nothing is looked up by it.
    const domain::CollectionId collection(root);
    const domain::AssociationResult grouped = domain::StemAssociationResolver{}.resolve(
        collection, files, domain::AssociationConfig::defaults(), true);

    for (const domain::PhotoAsset& asset : grouped.assets) {
        // Anything with a preview is shown, ambiguous pairings and symlinks
        // included: those make a photo unsafe to move, not unsafe to look at.
        if (asset.preview() == nullptr) {
            ++result.withoutPreview;
            continue;
        }
        result.photos.append(ui::AssetPresentation::from(asset));
    }

    std::ranges::stable_sort(
        result.photos, [](const ui::AssetPresentation& lhs, const ui::AssetPresentation& rhs) {
            return domain::naturalLess(lhs.displayName, rhs.displayName);
        });
    return result;
}

} // namespace cullfinch::viewer
