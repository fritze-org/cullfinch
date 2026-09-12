// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/SelectionSnapshot.h>

#include <QCoreApplication>

namespace cullfinch::domain {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

/// Why this asset cannot enter a comparison, or an empty string when it can.
///
/// Split out of `selectEligible` so the decision is one flat chain rather than a
/// switch nested inside a branch inside the loop.
QString exclusionReason(const PhotoAsset& asset) {
    using enum PairingState;
    if (asset.disposition == Disposition::Reject) {
        return tr("Already marked for deletion. Unmark it first to compare it again.");
    }
    switch (asset.pairingState) {
    case Provisional:
        return tr("Still being scanned; its file group is not final yet.");
    case RawOnly:
        return tr("RAW only, with no JPG preview to display.");
    case Ambiguous:
        return tr("The file group needs resolution before it can be compared.");
    case Stale:
        return tr("The files changed on disk. Refresh the collection first.");
    case Resolved:
    case JpegOnly:
        break;
    }
    return asset.isComparable() ? QString() : tr("No displayable preview.");
}

} // namespace

SelectionSnapshot SelectionSnapshot::freeze(const CollectionId& collectionId,
                                            quint64 collectionRevision,
                                            const PhotoAssetList& assets) {
    SelectionSnapshot snapshot;
    snapshot.collectionId = collectionId;
    snapshot.collectionRevision = collectionRevision;
    snapshot.orderedAssetIds.reserve(assets.size());
    for (const PhotoAsset& asset : assets) {
        if (snapshot.membershipRevisions.contains(asset.id)) {
            continue;
        }
        snapshot.orderedAssetIds.append(asset.id);
        snapshot.membershipRevisions.insert(asset.id, asset.membershipRevision);
    }
    return snapshot;
}

int SelectionSnapshot::indexOf(const AssetId& id) const {
    return static_cast<int>(orderedAssetIds.indexOf(id));
}

SelectionEligibility selectEligible(const CollectionId& collectionId, quint64 collectionRevision,
                                    const PhotoAssetList& selected) {
    SelectionEligibility eligibility;
    PhotoAssetList accepted;
    accepted.reserve(selected.size());

    for (const PhotoAsset& asset : selected) {
        const QString reason = exclusionReason(asset);
        if (reason.isEmpty()) {
            accepted.append(asset);
        } else {
            eligibility.excluded.append(ExcludedAsset{asset.id, asset.displayName, reason});
        }
    }

    eligibility.snapshot = SelectionSnapshot::freeze(collectionId, collectionRevision, accepted);
    return eligibility;
}

} // namespace cullfinch::domain
