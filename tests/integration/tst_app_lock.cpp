// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/AppLock.h>
#include <cullfinch/infrastructure/Paths.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace cullfinch;

/// The writer lock as a second instance would meet it.
///
/// QLockFile is file-based, so two locks on the same root inside one process
/// behave exactly like two processes: the second cannot take it.
class TestAppLock : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void aSecondWriterIsRefusedAndToldWho();
    void releasingLetsTheNextWriterIn();
    void differentCollectionsDoNotContend();
    void theLockLivesBesideTheDatabaseNotInTheCollection();
    void aSymlinkToTheCollectionContendsForTheSameLock();

private:
    std::unique_ptr<QTemporaryDir> dataDirectory_;
    std::unique_ptr<QTemporaryDir> collection_;
};

void TestAppLock::init() {
    dataDirectory_ = std::make_unique<QTemporaryDir>();
    collection_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dataDirectory_->isValid());
    QVERIFY(collection_->isValid());
    infrastructure::Paths::overrideRoots(dataDirectory_->path(), dataDirectory_->path());
}

void TestAppLock::cleanup() {
    infrastructure::Paths::clearOverrides();
}

void TestAppLock::aSecondWriterIsRefusedAndToldWho() {
    infrastructure::AppLock first;
    QString holder;
    QVERIFY(first.acquire(collection_->path(), &holder));
    QVERIFY(first.isHeld());

    infrastructure::AppLock second;
    QVERIFY2(!second.acquire(collection_->path(), &holder),
             "two writers on one collection is exactly what the lock exists to prevent");
    QVERIFY(!second.isHeld());
    QVERIFY2(!holder.isEmpty(), "the refused instance must be able to say who has the lock");

    // Asking again for the root already held is idempotent.
    QVERIFY(first.acquire(collection_->path(), &holder));
}

void TestAppLock::releasingLetsTheNextWriterIn() {
    infrastructure::AppLock first;
    QVERIFY(first.acquire(collection_->path(), nullptr));

    infrastructure::AppLock second;
    QVERIFY(!second.acquire(collection_->path(), nullptr));

    first.release();
    QVERIFY(!first.isHeld());
    QVERIFY(second.acquire(collection_->path(), nullptr));
    QVERIFY(second.isHeld());
}

void TestAppLock::differentCollectionsDoNotContend() {
    const QTemporaryDir other;
    infrastructure::AppLock first;
    infrastructure::AppLock second;
    QVERIFY(first.acquire(collection_->path(), nullptr));
    QVERIFY(second.acquire(other.path(), nullptr));

    // Moving a lock to another root releases the one it held.
    QVERIFY(second.acquire(collection_->path(), nullptr) == false);
    QVERIFY(second.isHeld() == false);
    infrastructure::AppLock third;
    QVERIFY2(third.acquire(other.path(), nullptr), "the abandoned root must be free again");
}

void TestAppLock::theLockLivesBesideTheDatabaseNotInTheCollection() {
    infrastructure::AppLock lock;
    QVERIFY(lock.acquire(collection_->path(), nullptr));

    // Browsing a read-only photo directory must never require writing into it.
    const QString lockFile = infrastructure::AppLock::lockFileFor(collection_->path());
    QVERIFY(QFileInfo::exists(lockFile));
    QVERIFY(lockFile.startsWith(QFileInfo(dataDirectory_->path()).absoluteFilePath()));
    QVERIFY(!lockFile.startsWith(QFileInfo(collection_->path()).absoluteFilePath()));
}

void TestAppLock::aSymlinkToTheCollectionContendsForTheSameLock() {
    const QTemporaryDir elsewhere;
    const QString link = QDir(elsewhere.path()).absoluteFilePath(QStringLiteral("photos-link"));
    QVERIFY(QFile::link(collection_->path(), link));

    // Two spellings of one directory are one collection; a lock that told
    // them apart would let two writers in.
    QCOMPARE(infrastructure::AppLock::lockFileFor(link),
             infrastructure::AppLock::lockFileFor(collection_->path()));

    infrastructure::AppLock direct;
    QVERIFY(direct.acquire(collection_->path(), nullptr));
    infrastructure::AppLock viaLink;
    QString holder;
    QVERIFY2(!viaLink.acquire(link, &holder), "the symlinked spelling must be refused");
    QVERIFY(!holder.isEmpty());
}

QTEST_GUILESS_MAIN(TestAppLock)
#include "tst_app_lock.moc"
