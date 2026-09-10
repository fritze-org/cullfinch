// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/PhotoAsset.h>

#include <QHash>
#include <QString>

namespace cullfinch::ui {

/// Everything a comparison view is allowed to know about a photo.
///
/// Deliberately read-only display metadata plus the preview path: comparison
/// engines and their views receive asset identifiers and this, never writable
/// filesystem handles.
struct AssetPresentation {
    domain::AssetId id;
    domain::MemberId previewMemberId;
    QString displayName;
    QString previewPath;
    domain::FileFingerprint previewFingerprint;
    int rawCount = 0;
    QString pairingText;

    [[nodiscard]] static AssetPresentation from(const domain::PhotoAsset& asset);
};

using AssetPresentationMap = QHash<domain::AssetId, AssetPresentation>;

} // namespace cullfinch::ui
