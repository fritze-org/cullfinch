// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/AppLock.h>

#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

namespace cullfinch::infrastructure {

QString AppLock::lockFileFor(const QString& collectionRoot) {
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QFileInfo(collectionRoot).absoluteFilePath().toUtf8());
    const QString name = QStringLiteral("collection-%1.lock")
                             .arg(QString::fromLatin1(hash.result().toHex().left(16)));
    return QDir(Paths::applicationDataDirectory()).absoluteFilePath(name);
}

AppLock::~AppLock() {
    release();
}

bool AppLock::acquire(const QString& collectionRoot, QString* holder) {
    const QString root = QFileInfo(collectionRoot).absoluteFilePath();
    if (held_ && root_ == root) {
        return true;
    }
    release();

    root_ = root;
    lock_ = std::make_unique<QLockFile>(lockFileFor(root));
    // Never steal a lock automatically: a stale-looking lock may belong to a
    // process that is merely busy, and taking it would allow two writers.
    lock_->setStaleLockTime(0);
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
                "cullfinch", "Another Cullfinch window has this collection open.");
        }
    }
    lock_.reset();
    return false;
}

void AppLock::release() {
    if (held_ && lock_ != nullptr) {
        lock_->unlock();
    }
    lock_.reset();
    held_ = false;
    root_.clear();
}

} // namespace cullfinch::infrastructure
