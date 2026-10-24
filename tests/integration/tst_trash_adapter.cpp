// SPDX-License-Identifier: GPL-3.0-or-later
//
// infrastructure::QtTrashAdapter against the real platform code, without the
// real Trash.
//
// On freedesktop platforms the home Trash lives in $XDG_DATA_HOME, so pointing
// that variable into a temporary directory gives every case here a Trash of
// its own: the files, the Trash and its metadata all go when the directory
// does, and nobody's real Trash is involved. That is what lets this suite run
// everywhere, coverage included, where tst_native_trash needs an opt-in.
// Source and data home share one temporary directory, so they are always on
// the same filesystem and Qt never reaches for a per-mount Trash instead.
#include <cullfinch/infrastructure/TrashAdapter.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QtEnvironmentVariables>

using namespace cullfinch;

class TestTrashAdapter : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void refusesAPathThatIsNotThere();
    void createsAMissingDataHomeBeforeTrashing();
    void reusesAnExistingDataHome();

private:
    QByteArray previousDataHome_;
    bool hadDataHome_ = false;
};

void TestTrashAdapter::init() {
#if !defined(Q_OS_UNIX) || defined(Q_OS_DARWIN)
    QSKIP("$XDG_DATA_HOME only decides where the Trash is on freedesktop platforms; "
          "tst_native_trash covers the platform Trash here.");
#endif
    hadDataHome_ = qEnvironmentVariableIsSet("XDG_DATA_HOME");
    previousDataHome_ = qgetenv("XDG_DATA_HOME");
}

void TestTrashAdapter::cleanup() {
    if (hadDataHome_) {
        qputenv("XDG_DATA_HOME", previousDataHome_);
    } else {
        qunsetenv("XDG_DATA_HOME");
    }
}

/// A failure is reported as a failure, names the path, and invents no
/// destination.
void TestTrashAdapter::refusesAPathThatIsNotThere() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    qputenv("XDG_DATA_HOME", QFile::encodeName(root.filePath(QStringLiteral("share"))));

    infrastructure::QtTrashAdapter adapter;
    const QString absent = root.filePath(QStringLiteral("never-created"));
    QString resultingPath = QStringLiteral("untouched");
    QString error;
    QVERIFY(!adapter.moveToTrash(absent, &resultingPath, &error));
    QVERIFY2(error.contains(absent), qPrintable(error));
    QCOMPARE(resultingPath, QStringLiteral("untouched"));
}

/// A fresh account has no $XDG_DATA_HOME yet, and Qt's freedesktop Trash only
/// creates the last path component: without the adapter making the data home
/// first, Qt 6.11 fails the move with "Bad file descriptor". That is how a
/// GitHub runner, which has no ~/.local/share, failed tst_native_trash.
void TestTrashAdapter::createsAMissingDataHomeBeforeTrashing() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dataHome = root.filePath(QStringLiteral("never-made/share"));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));

    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("group")));
    const QString group = root.filePath(QStringLiteral("group"));
    QFile raw(QDir(group).absoluteFilePath(QStringLiteral("A.RAF")));
    const QByteArray contents = QByteArrayLiteral("RAWDATA");
    QVERIFY(raw.open(QIODevice::WriteOnly));
    QCOMPARE(raw.write(contents), contents.size());
    raw.close();

    infrastructure::QtTrashAdapter adapter;
    QString resultingPath;
    QString error;
    QVERIFY2(adapter.moveToTrash(group, &resultingPath, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(group));
    QCOMPARE(resultingPath, QDir(dataHome).absoluteFilePath(QStringLiteral("Trash/files/group")));
    QVERIFY(QFileInfo::exists(QDir(resultingPath).absoluteFilePath(QStringLiteral("A.RAF"))));

    // The data home holds more than the Trash; the XDG specification asks for
    // a newly created one to be private to its owner.
    QCOMPARE(QFile::permissions(dataHome) &
                 (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                  QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther),
             QFileDevice::Permissions());
}

/// An existing data home is used as it is: its permissions are the user's
/// business, not the adapter's.
void TestTrashAdapter::reusesAnExistingDataHome() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString dataHome = root.filePath(QStringLiteral("share"));
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral("share")));
    const QFileDevice::Permissions before = QFile::permissions(dataHome);
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));

    const QString photo = root.filePath(QStringLiteral("A.JPG"));
    QFile file(photo);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    infrastructure::QtTrashAdapter adapter;
    QString resultingPath;
    QString error;
    QVERIFY2(adapter.moveToTrash(photo, &resultingPath, &error), qPrintable(error));
    QCOMPARE(resultingPath, QDir(dataHome).absoluteFilePath(QStringLiteral("Trash/files/A.JPG")));
    QCOMPARE(QFile::permissions(dataHome), before);
}

QTEST_MAIN(TestTrashAdapter)
#include "tst_trash_adapter.moc"
