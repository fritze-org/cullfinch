// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/AppLock.h>

#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace cullfinch::infrastructure {

AppLock::AppLock(const QString& collectionRoot) {
    // The lock lives beside the metadata, not in the collection: writing to the
    // photo directory must never be a precondition for browsing it.
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QFileInfo(collectionRoot).absoluteFilePath().toUtf8());
    const QString name = QStringLiteral("collection-%1.lock")
                             .arg(QString::fromLatin1(hash.result().toHex().left(16)));
    lock_ =
        std::make_unique<QLockFile>(QDir(Paths::applicationDataDirectory()).absoluteFilePath(name));
    lock_->setStaleLockTime(0); // Never steal a lock automatically.
}

AppLock::~AppLock() {
    release();
}

bool AppLock::acquire(QString* holder) {
    if (held_) {
        return true;
    }
    if (lock_->tryLock(0)) {
        held_ = true;
        return true;
    }

    if (holder != nullptr) {
        qint64 pid = 0;
        QString hostname;
        QString application;
        if (lock_->getLockInfo(&pid, &hostname, &application)) {
            *holder = QCoreApplication::translate("cullfinch",
                                                  "%1 (process %2) on %3 has this collection open.")
                          .arg(application)
                          .arg(pid)
                          .arg(hostname);
        } else {
            *holder = QCoreApplication::translate(
                "cullfinch", "Another cullfinch window has this collection open.");
        }
    }
    return false;
}

void AppLock::release() {
    if (held_ && lock_ != nullptr) {
        lock_->unlock();
    }
    held_ = false;
}

} // namespace cullfinch::infrastructure
