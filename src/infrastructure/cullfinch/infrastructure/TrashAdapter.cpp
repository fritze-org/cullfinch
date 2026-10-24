// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/TrashAdapter.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace cullfinch::infrastructure {

namespace {

/// Make sure the directory the freedesktop home Trash lives in exists.
///
/// Qt opens $XDG_DATA_HOME/Trash with openat() and creates it with mkdirat(),
/// which creates one level only. On an account where $XDG_DATA_HOME
/// (~/.local/share) has never been made -- a fresh user, a CI runner -- that
/// mkdirat() fails with ENOENT, Qt goes on to fstat() the -1 it got back, and
/// the move fails with "Bad file descriptor" instead of creating the Trash the
/// specification says to create. The XDG base directory specification allows,
/// indeed asks, that a missing data home be created with 0700 before writing
/// into it, so doing that here is the fix rather than a workaround of a
/// different behaviour. A failure is left for Qt to report.
void ensureHomeTrashParentExists() {
#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dataHome.isEmpty() || QFileInfo::exists(dataHome)) {
        return;
    }
    // mkpath() creates the missing ancestors with the process umask; the leaf
    // is tightened afterwards to the 0700 the specification asks for.
    if (QDir().mkpath(dataHome)) {
        QFile::setPermissions(dataHome, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                            QFileDevice::ExeOwner);
    }
#endif
}

} // namespace

bool QtTrashAdapter::moveToTrash(const QString& path, QString* resultingPath, QString* error) {
    if (!QFileInfo::exists(path)) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("cullfinch", "'%1' no longer exists.").arg(path);
        }
        return false;
    }

    ensureHomeTrashParentExists();

    // The non-static overload is used so QFile::errorString() explains a
    // failure, and QFile::fileName() reports the Trash destination when the
    // platform supplies one.
    QFile source(path);
    if (!source.moveToTrash()) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("cullfinch", "Moving '%1' to Trash failed: %2")
                         .arg(path, source.errorString());
        }
        return false;
    }

    if (resultingPath != nullptr) {
        // Documented as filesystem-dependent: an empty value is normal, not an
        // error, and recovery relies on the manifest instead.
        *resultingPath = source.fileName();
    }
    return true;
}

} // namespace cullfinch::infrastructure
