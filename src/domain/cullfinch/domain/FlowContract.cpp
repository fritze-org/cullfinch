// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/FlowContract.h>

#include <QJsonArray>
#include <QJsonValue>

#include <utility>

namespace cullfinch::domain {

bool FlowOptions::boolean(const QString& key, bool fallback) const {
    const QJsonValue value = values.value(key);
    return value.isBool() ? value.toBool() : fallback;
}

QString FlowOptions::string(const QString& key, const QString& fallback) const {
    const QJsonValue value = values.value(key);
    return value.isString() ? value.toString() : fallback;
}

int FlowOptions::integer(const QString& key, int fallback) const {
    const QJsonValue value = values.value(key);
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

QJsonArray toJsonArray(const QList<AssetId>& ids) {
    QJsonArray array;
    for (const AssetId& id : ids) {
        array.append(id.toString());
    }
    return array;
}

QList<AssetId> assetIdsFromJson(const QJsonArray& array) {
    QList<AssetId> ids;
    ids.reserve(array.size());
    for (const auto& value : array) {
        if (value.isString()) {
            ids.append(AssetId(value.toString()));
        }
    }
    return ids;
}

RestoreResult restoreFlowState(const VersionedFlowState& saved, const QString& flowId,
                               int stateSchemaVersion, const QString& wrongFlowMessage,
                               const QString& wrongSchemaMessage) {
    if (saved.flowId != flowId) {
        return RestoreResult::failure(wrongFlowMessage);
    }
    if (saved.schemaVersion != stateSchemaVersion) {
        // An unknown or newer state version must never be interpreted. The
        // record is preserved and reported as incompatible instead.
        return RestoreResult::failure(wrongSchemaMessage);
    }

    FlowState state;
    state.flowId = saved.flowId;
    state.schemaVersion = saved.schemaVersion;
    state.revision = saved.revision;
    state.payload = saved.payload;

    RestoreResult result;
    result.restored = true;
    result.state = std::move(state);
    return result;
}

} // namespace cullfinch::domain
