// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/AssetBuilder.h>

namespace cullfinch::testsupport {

AssetBuilder::AssetBuilder(QString stem) {
    asset_.stem = std::move(stem);
    asset_.displayName = asset_.stem;
    asset_.id = domain::AssetId(QStringLiteral("asset-") + asset_.stem);
    asset_.collectionId = domain::CollectionId(QStringLiteral("collection"));
    asset_.pairingState = domain::PairingState::JpegOnly;
    asset_.membershipRevision = 1;
}

AssetBuilder& AssetBuilder::withJpeg(qint64 sizeBytes) {
    domain::FileMember member;
    member.id = domain::MemberId(asset_.stem + QStringLiteral("-jpeg"));
    member.role = domain::MemberRole::Jpeg;
    member.fileName = asset_.stem + QStringLiteral(".JPG");
    member.absolutePath = QStringLiteral("/photos/") + member.fileName;
    member.extensionLower = QStringLiteral("jpg");
    member.fingerprint.sizeBytes = sizeBytes;
    member.fingerprint.modifiedMsecsUtc = 1'700'000'000'000;
    asset_.members.append(member);
    asset_.previewMemberId = member.id;
    ++memberCounter_;
    return *this;
}

AssetBuilder& AssetBuilder::withRaw(const QString& extension, qint64 sizeBytes) {
    domain::FileMember member;
    member.id = domain::MemberId(asset_.stem + QStringLiteral("-raw-") + extension);
    member.role = domain::MemberRole::Raw;
    member.fileName = asset_.stem + QLatin1Char('.') + extension.toUpper();
    member.absolutePath = QStringLiteral("/photos/") + member.fileName;
    member.extensionLower = extension.toLower();
    member.fingerprint.sizeBytes = sizeBytes;
    member.fingerprint.modifiedMsecsUtc = 1'700'000'000'000;
    asset_.members.append(member);
    asset_.pairingState = domain::PairingState::Resolved;
    ++memberCounter_;
    return *this;
}

AssetBuilder& AssetBuilder::withPairing(domain::PairingState state) {
    asset_.pairingState = state;
    return *this;
}

AssetBuilder& AssetBuilder::withDisposition(domain::Disposition disposition) {
    asset_.disposition = disposition;
    return *this;
}

AssetBuilder& AssetBuilder::withOperationsBlocked(bool blocked) {
    asset_.operationsBlocked = blocked;
    return *this;
}

AssetBuilder& AssetBuilder::withCollection(const domain::CollectionId& id) {
    asset_.collectionId = id;
    return *this;
}

domain::PhotoAsset AssetBuilder::build() const {
    return asset_;
}

domain::PhotoAssetList AssetBuilder::resolvedSeries(int count) {
    domain::PhotoAssetList assets;
    assets.reserve(count);
    for (int index = 0; index < count; ++index) {
        assets.append(AssetBuilder(QStringLiteral("IMG_%1").arg(index, 4, 10, QLatin1Char('0')))
                          .withJpeg()
                          .withRaw()
                          .build());
    }
    return assets;
}

} // namespace cullfinch::testsupport
