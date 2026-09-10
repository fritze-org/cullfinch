// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/FakeTrashAdapter.h>

#include <QDir>
#include <QFileInfo>

namespace cullfinch::testsupport {

bool FakeTrashAdapter::moveToTrash(const QString& path, QString* resultingPath, QString* error) {
    ++callCount_;

    if (failuresRemaining_ > 0) {
        --failuresRemaining_;
        if (error != nullptr) {
            *error = QStringLiteral("fake trash failure");
        }
        return false;
    }

    if (!QFileInfo::exists(path)) {
        if (error != nullptr) {
            *error = QStringLiteral("path does not exist");
        }
        return false;
    }

    // Move it out of the collection so the scanner no longer sees it, exactly
    // as a real Trash would.
    const QString destination =
        QDir::temp().absoluteFilePath(QStringLiteral("cullfinch-fake-trash/%1-%2")
                                          .arg(callCount_)
                                          .arg(QFileInfo(path).fileName()));
    QDir().mkpath(QFileInfo(destination).absolutePath());
    if (!QDir().rename(path, destination)) {
        if (error != nullptr) {
            *error = QStringLiteral("fake trash could not move the directory");
        }
        return false;
    }

    trashed_.append(path);
    if (resultingPath != nullptr) {
        *resultingPath = reportsPath_ ? destination : QString();
    }
    return true;
}

} // namespace cullfinch::testsupport
