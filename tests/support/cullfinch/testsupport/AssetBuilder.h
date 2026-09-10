// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/PhotoAsset.h>

namespace cullfinch::testsupport {

/// Builds photo assets without touching a filesystem, for pure domain tests.
class AssetBuilder {
public:
    explicit AssetBuilder(QString stem);

    AssetBuilder& withJpeg(qint64 sizeBytes = 1024);
    AssetBuilder& withRaw(const QString& extension = QStringLiteral("raf"),
                          qint64 sizeBytes = 8192);
    AssetBuilder& withPairing(domain::PairingState state);
    AssetBuilder& withDisposition(domain::Disposition disposition);
    AssetBuilder& withOperationsBlocked(bool blocked);
    AssetBuilder& withCollection(const domain::CollectionId& id);

    [[nodiscard]] domain::PhotoAsset build() const;

    /// A convenience for tests that need N ordinary JPG+RAW photos.
    [[nodiscard]] static domain::PhotoAssetList resolvedSeries(int count);

private:
    domain::PhotoAsset asset_;
    int memberCounter_ = 0;
};

} // namespace cullfinch::testsupport
