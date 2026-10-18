// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropSequence.h>

#include <cullfinch/cropper/CropWriter.h>
#include <cullfinch/cropper/Sidecar.h>
#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace cullfinch::cropper {

bool isAlreadyCropped(const CropTarget& target) {
    if (!target.jpegPath.isEmpty() && QFileInfo::exists(cropOutputPath(target.jpegPath))) {
        return true;
    }
    return std::ranges::any_of(target.rawPaths, hasRecordedCrop);
}

CropPlan planCrops(const QString& directory, bool recursive, bool redo) {
    CropPlan plan;
    plan.recursive = recursive;
    const QString root = QDir(directory).absolutePath();

    // The scanner and the pairing policy the browser uses, as the wall viewer
    // does, rather than a listing of *.jpg: a JPG and its RAW are one step,
    // and pairing stays inside a directory. The wall's own loader is not
    // reused because it hands out presentations only, and a crop needs to
    // know where the RAW companions are.
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(root, recursive, &plan.error);
    if (!plan.error.isEmpty()) {
        return plan;
    }
    const domain::CollectionId collection(root);
    const domain::AssociationResult grouped = domain::StemAssociationResolver{}.resolve(
        collection, files, domain::AssociationConfig::defaults(), true);

    // Already in the order to show: by directory, then by natural stem.
    for (const domain::PhotoAsset& asset : grouped.assets) {
        if (isCropOutputStem(asset.stem)) {
            continue;
        }
        const domain::FileMember* preview = asset.preview();
        if (preview == nullptr) {
            ++plan.withoutPreview;
            continue;
        }
        CropTarget target;
        target.presentation = ui::AssetPresentation::from(asset);
        target.jpegPath = preview->absolutePath;
        for (const domain::FileMember& raw : asset.membersWithRole(domain::MemberRole::Raw)) {
            target.rawPaths.append(raw.absolutePath);
        }
        if (!redo && isAlreadyCropped(target)) {
            ++plan.alreadyCropped;
            continue;
        }
        plan.targets.append(target);
    }
    return plan;
}

} // namespace cullfinch::cropper
