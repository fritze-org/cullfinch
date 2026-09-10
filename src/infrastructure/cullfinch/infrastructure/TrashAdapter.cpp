// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/TrashAdapter.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

namespace cullfinch::infrastructure {

bool QtTrashAdapter::moveToTrash(const QString& path, QString* resultingPath, QString* error) {
    if (!QFileInfo::exists(path)) {
        if (error != nullptr) {
            *error = QCoreApplication::translate("cullfinch", "'%1' no longer exists.").arg(path);
        }
        return false;
    }

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
