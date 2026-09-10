// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/FlowContract.h>

#include <QJsonArray>
#include <QJsonValue>

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
    for (const QJsonValue& value : array) {
        if (value.isString()) {
            ids.append(AssetId(value.toString()));
        }
    }
    return ids;
}

} // namespace cullfinch::domain
