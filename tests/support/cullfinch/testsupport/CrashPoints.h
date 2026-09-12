// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QLatin1String>
#include <QStringView>

#include <optional>

namespace cullfinch::testsupport {

/// Where a crash-injection run stops.
///
/// Shared by the helper that dies there and the suite that tells it to, because the two sides agree
/// through a string in an environment variable: an unrecognised step would otherwise let the helper
/// run to completion while the suite still asserted against a half-finished operation, and the
/// suite would pass. Naming the steps once makes that a compile error instead.
///
/// Every step is a point where the filesystem and the journal disagree in a different way, which is
/// what makes each one worth injecting.
enum class CrashPoint {
    None,        ///< Run to completion. The control case: it proves the helper itself works.
    FirstRename, ///< A file has been renamed into staging; the journal has not been told yet.
    Staged,      ///< The whole group is in staging and the journal durably says so.
    Trashing,    ///< The intent to Trash is durable; Trash has not been called.
    Trashed      ///< Trash has taken the group; its outcome never reached the journal.
};

/// The exit status a killed helper reports. Distinct from every status it produces on its own, so
/// the suite can tell "died where it was told to" from "failed on the way there".
inline constexpr int kCrashExitCode = 99;

[[nodiscard]] inline QLatin1String crashPointToken(CrashPoint point) {
    switch (point) {
    case CrashPoint::None:
        return QLatin1String("none");
    case CrashPoint::FirstRename:
        return QLatin1String("first-rename");
    case CrashPoint::Staged:
        return QLatin1String("staged");
    case CrashPoint::Trashing:
        return QLatin1String("trashing");
    case CrashPoint::Trashed:
        return QLatin1String("trashed");
    }
    return QLatin1String("none");
}

/// @return nullopt for a token no build of the helper knows, which is a usage error rather than a
///         reason to run the operation to completion.
[[nodiscard]] inline std::optional<CrashPoint> crashPointFromToken(QStringView token) {
    for (const CrashPoint point : {CrashPoint::None, CrashPoint::FirstRename, CrashPoint::Staged,
                                   CrashPoint::Trashing, CrashPoint::Trashed}) {
        if (token == crashPointToken(point)) {
            return point;
        }
    }
    return std::nullopt;
}

} // namespace cullfinch::testsupport
