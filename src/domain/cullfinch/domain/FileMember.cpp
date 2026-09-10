// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/FileMember.h>

#include <QCoreApplication>

namespace cullfinch::domain {

QString memberRoleName(MemberRole role) {
    switch (role) {
    case MemberRole::Jpeg:
        return QCoreApplication::translate("cullfinch", "JPEG");
    case MemberRole::Raw:
        return QCoreApplication::translate("cullfinch", "RAW");
    case MemberRole::Sidecar:
        return QCoreApplication::translate("cullfinch", "Sidecar");
    case MemberRole::Unknown:
        break;
    }
    return QCoreApplication::translate("cullfinch", "Unclassified");
}

} // namespace cullfinch::domain
