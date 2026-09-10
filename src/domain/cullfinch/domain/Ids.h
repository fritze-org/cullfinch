// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QHash>
#include <QMetaType>
#include <QString>
#include <QUuid>

#include <utility>

namespace cullfinch::domain {

/// A phantom-typed identifier.
///
/// Model rows, sort positions and filenames are not stable identities
/// (invariant 8), so every long-lived reference in the domain uses one of the
/// aliases below. Distinct tags do not convert into each other, which is what
/// stops an asset identifier being passed where a member identifier is meant.
template<typename Tag>
class StrongId {
public:
    StrongId() = default;
    explicit StrongId(QString value) : value_(std::move(value)) {}

    /// A fresh, process-unique identifier.
    [[nodiscard]] static StrongId generate() {
        return StrongId(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    [[nodiscard]] bool isValid() const noexcept { return !value_.isEmpty(); }
    [[nodiscard]] const QString& toString() const noexcept { return value_; }

    friend bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend bool operator<(const StrongId& lhs, const StrongId& rhs) noexcept {
        return lhs.value_ < rhs.value_;
    }

private:
    QString value_;
};

template<typename Tag>
inline size_t qHash(const StrongId<Tag>& id, size_t seed = 0) noexcept {
    return qHash(id.toString(), seed);
}

struct CollectionTag;
struct AssetTag;
struct MemberTag;
struct SessionTag;
struct OperationTag;

using CollectionId = StrongId<CollectionTag>;
using AssetId = StrongId<AssetTag>;
using MemberId = StrongId<MemberTag>;
using SessionId = StrongId<SessionTag>;
using OperationId = StrongId<OperationTag>;

} // namespace cullfinch::domain

// Registered so identifiers survive queued signal delivery between the worker
// threads and the GUI thread.
Q_DECLARE_METATYPE(cullfinch::domain::CollectionId)
Q_DECLARE_METATYPE(cullfinch::domain::AssetId)
Q_DECLARE_METATYPE(cullfinch::domain::MemberId)
Q_DECLARE_METATYPE(cullfinch::domain::SessionId)
Q_DECLARE_METATYPE(cullfinch::domain::OperationId)
