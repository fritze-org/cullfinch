// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/AssetPresentation.h>

namespace cullfinch::ui {

AssetPresentation AssetPresentation::from(const domain::PhotoAsset& asset) {
    AssetPresentation presentation;
    presentation.id = asset.id;
    presentation.displayName = asset.displayName;
    presentation.rawCount = asset.rawCount();
    presentation.pairingText = domain::pairingStateName(asset.pairingState);

    const domain::FileMember* preview = asset.preview();
    if (preview != nullptr) {
        presentation.previewMemberId = preview->id;
        presentation.previewPath = preview->absolutePath;
        presentation.previewFingerprint = preview->fingerprint;
    }
    return presentation;
}

} // namespace cullfinch::ui
