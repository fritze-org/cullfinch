// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/Ids.h>
#include <cullfinch/domain/PhotoAsset.h>

#include <QHash>
#include <QList>

namespace cullfinch::domain {

/// The frozen input to a comparison.
///
/// Once a flow starts, new scan results, thumbnail completions and later sort
/// changes must not alter its logical input. The recorded membership revisions
/// are what lets a resumed session notice that a group moved underneath it.
struct SelectionSnapshot {
    CollectionId collectionId;
    quint64 collectionRevision = 0;
    QList<AssetId> orderedAssetIds;
    QHash<AssetId, quint64> membershipRevisions;

    [[nodiscard]] static SelectionSnapshot freeze(const CollectionId& collectionId,
                                                  quint64 collectionRevision,
                                                  const PhotoAssetList& assets);

    [[nodiscard]] bool contains(const AssetId& id) const {
        return membershipRevisions.contains(id);
    }
    [[nodiscard]] qsizetype size() const { return orderedAssetIds.size(); }
    [[nodiscard]] bool isEmpty() const { return orderedAssetIds.isEmpty(); }
    [[nodiscard]] int indexOf(const AssetId& id) const;
};

/// Why a selected asset could not enter a flow.
struct ExcludedAsset {
    AssetId id;
    QString displayName;
    QString reason;
};

/// The result of turning a user selection into a flow input.
struct SelectionEligibility {
    SelectionSnapshot snapshot;
    QList<ExcludedAsset> excluded;

    [[nodiscard]] bool hasExclusions() const { return !excluded.isEmpty(); }
};

/// Split a selection into the assets a comparison may actually receive and the
/// ones it may not, with an explanation for each exclusion.
///
/// Already-rejected assets are excluded unless the user first unmarks them.
[[nodiscard]] SelectionEligibility selectEligible(const CollectionId& collectionId,
                                                  quint64 collectionRevision,
                                                  const PhotoAssetList& selected);

} // namespace cullfinch::domain
