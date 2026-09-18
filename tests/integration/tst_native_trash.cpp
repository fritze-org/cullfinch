// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only suite that asks the platform's real Trash for anything.
//
// Every other suite uses FakeTrashAdapter, which leaves two things unverified:
// infrastructure::QtTrashAdapter itself, and the assumption recovery is built
// on -- that when the platform reports a Trash path, that path still holds the
// group directory, manifest and all. Neither can be checked against a double,
// because the double is what implements them correctly by construction.
//
// Opt-in, because it writes into the Trash of whoever runs it:
//
//   CULLFINCH_NATIVE_TRASH_TESTS=1 ctest --preset dev-fast --label-regex native-trash
//
// What it will and will not touch:
//
//   * Fixtures are created under $HOME (override with CULLFINCH_NATIVE_TRASH_ROOT),
//     not under the system temp directory. On Linux the freedesktop Trash is
//     per-mount, and /tmp is commonly a tmpfs with no trash directory at all,
//     so a fixture there would exercise a failure path rather than the one
//     people actually use.
//   * The Trash is never enumerated, listed or emptied. The only thing removed
//     is the single directory this suite put there, by the exact path the
//     platform reported for it -- and if the platform reported no path,
//     nothing is removed at all and the test says so.
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/infrastructure/TrashAdapter.h>

#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/domain/OperationPlan.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QtEnvironmentVariables>

#include <optional>

using namespace cullfinch;

namespace {

/// Where fixtures are created. $HOME by default, so the files live on the
/// volume whose Trash the platform would really use for the user's photos.
QString fixtureRoot() {
    const QString configured = qEnvironmentVariable("CULLFINCH_NATIVE_TRASH_ROOT");
    return configured.isEmpty() ? QDir::homePath() : configured;
}

/// A disposable directory under the fixture root, named so that anything left
/// behind by a killed run is recognisable rather than mysterious.
class NativeFixture {
public:
    NativeFixture() {
        static int serial = 0;
        QDir root(fixtureRoot());
        const QString name = QStringLiteral(".cullfinch-native-trash-test-%1-%2")
                                 .arg(QCoreApplication::applicationPid())
                                 .arg(++serial);
        path_ = root.absoluteFilePath(name);
        valid_ = root.mkpath(name);
    }

    NativeFixture(const NativeFixture&) = delete;
    NativeFixture& operator=(const NativeFixture&) = delete;
    NativeFixture(NativeFixture&&) = delete;
    NativeFixture& operator=(NativeFixture&&) = delete;

    ~NativeFixture() {
        if (valid_) {
            QDir(path_).removeRecursively();
        }
    }

    [[nodiscard]] bool isValid() const { return valid_; }
    [[nodiscard]] QString path() const { return path_; }
    [[nodiscard]] QString filePath(const QString& relative) const {
        return QDir(path_).absoluteFilePath(relative);
    }

    /// cullfinch never decodes a companion's bytes, and this suite never
    /// decodes anything: what matters is that real files of a known size move.
    bool write(const QString& relative, const QByteArray& contents) const {
        QFile file(filePath(relative));
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        return file.write(contents) == contents.size();
    }

private:
    QString path_;
    bool valid_ = false;
};

/// Remove exactly the directory the platform reported, and on the freedesktop
/// implementation the one metadata file that belongs to it.
///
/// Deliberately not a "clean the Trash" helper: it takes a single path, checks
/// that it is the one handed back by the call under test, and touches nothing
/// else. A path that was never reported means nothing is removed.
void removeFromTrash(const QString& trashPath) {
    if (trashPath.isEmpty() || !QFileInfo::exists(trashPath)) {
        return;
    }

    // The freedesktop spec keeps <trash>/files/<name> beside
    // <trash>/info/<name>.trashinfo. Leaving the second behind would make the
    // suite litter someone's Trash with dangling entries.
    const QFileInfo info(trashPath);
    const QDir filesDir = info.absoluteDir();
    if (filesDir.dirName() == QLatin1String("files")) {
        QDir infoDir(filesDir);
        if (infoDir.cdUp() && infoDir.cd(QStringLiteral("info"))) {
            QFile::remove(infoDir.absoluteFilePath(info.fileName() + QStringLiteral(".trashinfo")));
        }
    }

    if (info.isDir()) {
        QDir(trashPath).removeRecursively();
    } else {
        QFile::remove(trashPath);
    }
}

domain::PhotoAssetList scan(const QString& root) {
    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(root, false, &error);
    const domain::StemAssociationResolver resolver;
    return resolver
        .resolve(domain::CollectionId(QStringLiteral("native")), files,
                 domain::AssociationConfig::defaults(), true)
        .assets;
}

/// The record the controller hands the executor: the plan, every member at
/// "planned", nothing moved.
application::OperationRecord recordFor(const domain::OperationPlan& plan) {
    application::OperationRecord record;
    record.plan = plan;
    for (const domain::PlannedGroup& group : plan.groups) {
        for (const domain::PlannedMember& member : group.members) {
            record.members.append(application::OperationMemberRecord{member.memberId,
                                                                     group.assetId,
                                                                     member.sourcePath,
                                                                     {},
                                                                     QStringLiteral("planned"),
                                                                     {}});
        }
    }
    return record;
}

QStringList stepsOf(const application::OperationRecord& record) {
    QStringList steps;
    steps.reserve(record.members.size());
    for (const application::OperationMemberRecord& member : record.members) {
        steps.append(member.lastDurableStep);
    }
    return steps;
}

} // namespace

class TestNativeTrash : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void refusesAPathThatIsNotThere();
    void trashesADirectoryWholeAndReportsWhereItWent();
    void recoveryConfirmsTheOutcomeFromTheReportedPath();
    void cleanupTestCase();

private:
    /// Every path this suite handed to Trash, removed again at the end. Nothing
    /// else in the Trash is looked at, let alone removed.
    QStringList trashed_;
};

void TestNativeTrash::initTestCase() {
    if (qEnvironmentVariable("CULLFINCH_NATIVE_TRASH_TESTS") != QLatin1String("1")) {
        QSKIP("Set CULLFINCH_NATIVE_TRASH_TESTS=1 to run the suite that uses the real Trash. "
              "It writes into the Trash of whoever runs it and removes only what it put there.");
    }

    const NativeFixture probe;
    QVERIFY2(probe.isValid(),
             qPrintable(QStringLiteral("no writable fixture root under '%1'; set "
                                       "CULLFINCH_NATIVE_TRASH_ROOT to a directory on a volume "
                                       "whose Trash this account may use")
                            .arg(fixtureRoot())));
}

void TestNativeTrash::cleanupTestCase() {
    for (const QString& path : trashed_) {
        removeFromTrash(path);
    }
}

/// The guard in front of every other case: the adapter reports a failure as a
/// failure and names the path, rather than reporting success for a file that
/// was never there.
void TestNativeTrash::refusesAPathThatIsNotThere() {
    const NativeFixture fixture;
    QVERIFY(fixture.isValid());

    infrastructure::QtTrashAdapter adapter;
    const QString absent = fixture.filePath(QStringLiteral("never-created"));

    QString resultingPath = QStringLiteral("untouched");
    QString error;
    QVERIFY(!adapter.moveToTrash(absent, &resultingPath, &error));
    QVERIFY2(error.contains(absent), qPrintable(error));
    // A failed call must not invent a destination.
    QCOMPARE(resultingPath, QStringLiteral("untouched"));
}

/// The adapter's contract against the real platform: a directory goes as one
/// unit, the source is gone afterwards, and where a path is reported it holds
/// the contents that went in.
void TestNativeTrash::trashesADirectoryWholeAndReportsWhereItWent() {
    const NativeFixture fixture;
    QVERIFY(fixture.isValid());

    const QString groupName = QStringLiteral("group");
    QVERIFY(QDir(fixture.path()).mkpath(groupName));
    const QString group = fixture.filePath(groupName);

    const QByteArray manifest = QByteArrayLiteral(R"({"assetId":"a1","members":[]})");
    QFile manifestFile(
        QDir(group).absoluteFilePath(infrastructure::StagingExecutor::manifestFileName()));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    QCOMPARE(manifestFile.write(manifest), manifest.size());
    manifestFile.close();
    QVERIFY(fixture.write(QStringLiteral("group/A.RAF"), QByteArrayLiteral("RAWDATA")));

    infrastructure::QtTrashAdapter adapter;
    QString resultingPath;
    QString error;
    QVERIFY2(adapter.moveToTrash(group, &resultingPath, &error), qPrintable(error));
    trashed_.append(resultingPath);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Whatever the platform does with it, it is no longer where it was.
    QVERIFY(!QFileInfo::exists(group));

    if (resultingPath.isEmpty()) {
        // Documented as filesystem-dependent, so this is an outcome to record
        // rather than a failure -- and the case recovery has to survive. Note
        // that nothing can be cleaned up here: see the file header.
        qInfo("this platform reported no Trash path; the 'no path' branch is the one in force "
              "here, and the directory could not be removed from Trash by this suite");
        return;
    }

    QVERIFY2(QFileInfo::exists(resultingPath), qPrintable(resultingPath));
    QVERIFY(QFileInfo(resultingPath).isDir());
    QCOMPARE(QFile(QDir(resultingPath)
                       .absoluteFilePath(infrastructure::StagingExecutor::manifestFileName()))
                 .size(),
             manifest.size());
    QVERIFY(QFileInfo::exists(QDir(resultingPath).absoluteFilePath(QStringLiteral("A.RAF"))));
}

/// The assumption recovery is built on, checked end to end against the real
/// platform: stage a group with the production executor, let it call the real
/// Trash, then take the journal back to the moment before the outcome was
/// recorded and make recover() work out what happened from the Trash path
/// alone.
///
/// Both outcomes are correct behaviour and which one applies is the platform's
/// answer, not the test's: a platform that reports a path must be confirmed
/// from the manifest, and a platform that reports none must leave the group
/// uncertain for a person rather than guess.
void TestNativeTrash::recoveryConfirmsTheOutcomeFromTheReportedPath() {
    const NativeFixture fixture;
    QVERIFY(fixture.isValid());
    QVERIFY(fixture.write(QStringLiteral("A.JPG"), QByteArrayLiteral("JPEGDATA")));
    QVERIFY(fixture.write(QStringLiteral("A.RAF"), QByteArrayLiteral("RAWDATA")));

    domain::PhotoAssetList assets = scan(fixture.path());
    QCOMPARE(assets.size(), 1);
    assets.first().disposition = domain::Disposition::Reject;

    const QString stagingRoot =
        QDir(fixture.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("native")), 1, stagingRoot, assets);
    QVERIFY(!planning.hasBlockers());
    QCOMPARE(planning.plan.groups.size(), 1);

    infrastructure::QtTrashAdapter adapter;
    infrastructure::StagingExecutor executor(adapter);
    const application::OperationRecord executed =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), {});
    QVERIFY2(executed.error.isEmpty(), qPrintable(executed.error));
    trashed_.append(executed.trashPath);

    // The real Trash took the whole group directory: both members left the
    // collection, and staging is empty again.
    QCOMPARE(executed.state, domain::OperationState::Trashing);
    QCOMPARE(stepsOf(executed),
             QStringList({QStringLiteral("trashed"), QStringLiteral("trashed")}));
    QVERIFY(!QFileInfo::exists(fixture.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(!QFileInfo::exists(fixture.filePath(QStringLiteral("A.RAF"))));
    QVERIFY(!QFileInfo::exists(
        QDir(stagingRoot).absoluteFilePath(planning.plan.groups.first().stagingDirectoryName)));

    // Wind the journal back to the instant before the Trash outcome was
    // recorded: the directory is gone, but the record still says "staged".
    // This is the state a crash between the Trash call and its journal write
    // leaves behind, and all recovery has to go on is executed.trashPath.
    application::OperationRecord interrupted = executed;
    interrupted.state = domain::OperationState::Trashing;
    for (application::OperationMemberRecord& member : interrupted.members) {
        member.lastDurableStep = QStringLiteral("staged");
    }

    const application::OperationRecord recovered = executor.recover(interrupted);

    if (executed.trashPath.isEmpty()) {
        qInfo("this platform reported no Trash path; recovery must leave the group for a person");
        QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
        QCOMPARE(stepsOf(recovered),
                 QStringList({QStringLiteral("uncertain"), QStringLiteral("uncertain")}));
        QVERIFY(!recovered.error.isEmpty());
        return;
    }

    // The premise this whole mechanism rests on: the reported path still holds
    // this group's manifest, so the outcome can be settled without asking
    // anyone and without touching a file.
    QString manifestError;
    const std::optional<infrastructure::Manifest> inTrash =
        infrastructure::StagingExecutor::readManifest(executed.trashPath, &manifestError);
    if (!inTrash.has_value()) {
        QFAIL(qPrintable(manifestError));
    }
    QCOMPARE(inTrash->assetId, planning.plan.groups.first().assetId);
    QCOMPARE(inTrash->members.size(), 2);

    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(recovered.state, domain::OperationState::Completed);
    QCOMPARE(stepsOf(recovered),
             QStringList({QStringLiteral("trashed"), QStringLiteral("trashed")}));
}

QTEST_MAIN(TestNativeTrash)
#include "tst_native_trash.moc"
