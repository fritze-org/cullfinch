// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace cullfinch::domain {

/// Locale-independent natural ordering for filenames.
///
/// Digit runs compare numerically, so `IMG_9.JPG` sorts before `IMG_10.JPG`.
/// Deliberately not QCollator-based: browser order is frozen into selection
/// snapshots and reproduced in tests, so it must not depend on the user's
/// locale or on ICU availability.
///
/// Returns a negative value, zero, or a positive value like strcmp.
[[nodiscard]] int naturalCompare(const QString& lhs, const QString& rhs);

/// Strict weak ordering wrapper suitable for std::sort and QList::sort.
[[nodiscard]] inline bool naturalLess(const QString& lhs, const QString& rhs) {
    return naturalCompare(lhs, rhs) < 0;
}

} // namespace cullfinch::domain
