// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/FileMember.h>

#include <QCoreApplication>

namespace cullfinch::domain {

QString memberRoleName(MemberRole role) {
    using enum MemberRole;
    switch (role) {
    case Jpeg:
        return QCoreApplication::translate("cullfinch", "JPEG");
    case Raw:
        return QCoreApplication::translate("cullfinch", "RAW");
    case Sidecar:
        return QCoreApplication::translate("cullfinch", "Sidecar");
    case Unknown:
        break;
    }
    return QCoreApplication::translate("cullfinch", "Unclassified");
}

} // namespace cullfinch::domain
