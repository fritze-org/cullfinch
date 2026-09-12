// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/Ids.h>

#include <QList>
#include <QString>

namespace cullfinch::domain {

/// What a physical file contributes to a photo.
enum class MemberRole {
    Jpeg,    ///< The displayed preview.
    Raw,     ///< An opaque companion; cullfinch never decodes these bytes.
    Sidecar, ///< Metadata travelling with the group, when sidecars are enabled.
    Unknown  ///< Same stem, unrecognised extension. Blocks file operations.
};

[[nodiscard]] QString memberRoleName(MemberRole role);

/// Filesystem identity, where the platform can supply one.
///
/// Used to notice that a path now refers to a different file, and to detect
/// hardlink aliases so a single physical entry is never operated on twice.
struct NativeIdentity {
    quint64 device = 0;
    quint64 fileId = 0;
    bool known = false;

    friend bool operator==(const NativeIdentity& lhs, const NativeIdentity& rhs) noexcept {
        return lhs.known == rhs.known && lhs.device == rhs.device && lhs.fileId == rhs.fileId;
    }
};

/// The evidence used to decide whether a file is still the file we planned for.
struct FileFingerprint {
    qint64 sizeBytes = -1;
    qint64 modifiedMsecsUtc = -1;
    NativeIdentity native;

    [[nodiscard]] bool isKnown() const noexcept { return sizeBytes >= 0; }

    /// True when both fingerprints describe the same file contents as far as
    /// cheap metadata can tell. Unknown fingerprints never match.
    [[nodiscard]] bool matches(const FileFingerprint& other) const noexcept {
        if (!isKnown() || !other.isKnown()) {
            return false;
        }
        if (native.known && other.native.known && !(native == other.native)) {
            return false;
        }
        return sizeBytes == other.sizeBytes && modifiedMsecsUtc == other.modifiedMsecsUtc;
    }

    friend bool operator==(const FileFingerprint& lhs, const FileFingerprint& rhs) = default;
};

/// One physical file belonging to exactly one photo asset.
struct FileMember {
    MemberId id;
    MemberRole role = MemberRole::Unknown;
    QString absolutePath;
    QString fileName;
    QString extensionLower; ///< Without the leading dot, case-folded.
    FileFingerprint fingerprint;
    bool isSymlink = false;

    [[nodiscard]] bool isValid() const noexcept { return id.isValid() && !absolutePath.isEmpty(); }
};

using FileMemberList = QList<FileMember>;

} // namespace cullfinch::domain
