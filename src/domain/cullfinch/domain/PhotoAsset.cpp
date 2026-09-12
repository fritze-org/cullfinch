// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/PhotoAsset.h>

#include <QCoreApplication>

namespace cullfinch::domain {

QString pairingStateName(PairingState state) {
    using enum PairingState;
    switch (state) {
    case Provisional:
        return QCoreApplication::translate("cullfinch", "Scanning");
    case Resolved:
        return QCoreApplication::translate("cullfinch", "JPG + RAW");
    case JpegOnly:
        return QCoreApplication::translate("cullfinch", "JPG only");
    case RawOnly:
        return QCoreApplication::translate("cullfinch", "RAW only");
    case Ambiguous:
        return QCoreApplication::translate("cullfinch", "Needs resolution");
    case Stale:
        break;
    }
    return QCoreApplication::translate("cullfinch", "Changed on disk");
}

bool PhotoAsset::isComparable() const {
    if (pairingState != PairingState::Resolved && pairingState != PairingState::JpegOnly) {
        return false;
    }
    const FileMember* jpeg = preview();
    return jpeg != nullptr && jpeg->role == MemberRole::Jpeg;
}

bool PhotoAsset::isOperable() const {
    if (operationsBlocked) {
        return false;
    }
    using enum PairingState;
    switch (pairingState) {
    case Resolved:
    case JpegOnly:
    case RawOnly:
        return !members.isEmpty();
    case Provisional:
    case Ambiguous:
    case Stale:
        break;
    }
    return false;
}

const FileMember* PhotoAsset::preview() const {
    if (!previewMemberId.isValid()) {
        return nullptr;
    }
    return memberById(previewMemberId);
}

const FileMember* PhotoAsset::memberById(const MemberId& memberId) const {
    for (const FileMember& member : members) {
        if (member.id == memberId) {
            return &member;
        }
    }
    return nullptr;
}

FileMemberList PhotoAsset::membersWithRole(MemberRole role) const {
    FileMemberList result;
    for (const FileMember& member : members) {
        if (member.role == role) {
            result.append(member);
        }
    }
    return result;
}

int PhotoAsset::rawCount() const {
    int count = 0;
    for (const FileMember& member : members) {
        if (member.role == MemberRole::Raw) {
            ++count;
        }
    }
    return count;
}

qint64 PhotoAsset::totalBytes() const {
    qint64 total = 0;
    for (const FileMember& member : members) {
        if (member.fingerprint.sizeBytes > 0) {
            total += member.fingerprint.sizeBytes;
        }
    }
    return total;
}

} // namespace cullfinch::domain
