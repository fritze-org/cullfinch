// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/SelectionSnapshot.h>

#include <QCoreApplication>

namespace cullfinch::domain {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
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
        QString reason;
        if (asset.disposition == Disposition::Reject) {
            reason = tr("Already marked for deletion. Unmark it first to compare it again.");
        } else {
            switch (asset.pairingState) {
            case PairingState::Provisional:
                reason = tr("Still being scanned; its file group is not final yet.");
                break;
            case PairingState::RawOnly:
                reason = tr("RAW only, with no JPG preview to display.");
                break;
            case PairingState::Ambiguous:
                reason = tr("The file group needs resolution before it can be compared.");
                break;
            case PairingState::Stale:
                reason = tr("The files changed on disk. Refresh the collection first.");
                break;
            case PairingState::Resolved:
            case PairingState::JpegOnly:
                if (!asset.isComparable()) {
                    reason = tr("No displayable preview.");
                }
                break;
            }
        }

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
