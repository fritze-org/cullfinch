// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/OperationController.h>
#include <cullfinch/infrastructure/Paths.h>
#include <cullfinch/infrastructure/SqliteRepository.h>
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/testsupport/CrashPoints.h>
#include <cullfinch/testsupport/FakeTrashAdapter.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <optional>

#ifndef CULLFINCH_CRASH_HELPER
#error "CULLFINCH_CRASH_HELPER must name the crash-injection helper binary."
#endif

using namespace cullfinch;
using cullfinch::testsupport::CrashPoint;
using cullfinch::testsupport::FakeTrashAdapter;
using cullfinch::testsupport::TempCollection;

namespace {

/// The one photo every case operates on. Two members, so a crash between the two renames leaves a
/// group genuinely torn across staging and the collection.
QStringList photoFiles() {
    return {QStringLiteral("A.JPG"), QStringLiteral("A.RAF")};
}

int photoFileCount() {
    return static_cast<int>(photoFiles().size());
}

} // namespace

/// What is actually on disk after cullfinch dies mid-operation, and whether a new process can put
/// it right.
///
/// The suites next door simulate a crash by capturing journal snapshots in-process and handing them
/// to `recover()`. That exercises the state machine over a journal the test wrote itself. It cannot
/// show that the SQLite journal committed what the executor believed it had, that the manifest
/// survived the rename it describes, or that a half-moved group is still explainable to a process
/// that was not there when it happened.
///
/// So each case here runs a real operation in a child process, kills it at a named journal write
/// with `_exit()`, and then opens the same database the way a restarted cullfinch would. Nothing is
/// carried over in memory: everything asserted comes from the filesystem and the journal.
class TestCrashRecovery : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void anUninterruptedRunCompletesAndLeavesNothingUnfinished();
    void recoversFromACrashBetweenARenameAndItsJournal();
    void recoversFromACrashAfterTheWholeGroupReachedStaging();
    void recoversFromACrashAfterTheIntentToTrashWasJournalled();
    void reportsAnUncertainOutcomeAfterACrashBetweenTrashAndItsJournal();

private:
    /// Run the helper to the given step and confirm it died exactly there.
    void runHelper(CrashPoint point);
    /// Open the database again, the way a restarted application would: a fresh connection, the
    /// collection found rather than created, and a controller that knows nothing of the run.
    void reopen();
    /// All three, plus the single operation the crashed run left behind.
    ///
    /// A QVERIFY inside a helper returns from the helper, not from the test, so every caller checks
    /// that this got as far as building a controller and finding a group before reading either.
    void crashAndReopen(CrashPoint point, application::OperationRecord* record);
    /// Recover, and assert the whole photo came back: every file at its original path, nothing left
    /// in staging, and nothing sent to Trash. The plan is left retryable rather than recorded as
    /// completed, which would say the photo was deleted when it is still there.
    ///
    /// The three steps before Trash is called all have to end this way, which is the point: what
    /// differs between them is the state the journal was caught in, not what recovery owes the
    /// photo.
    void recoverPutsThePhotoBack(const application::OperationRecord& stored);

    [[nodiscard]] QString stagingRoot() const;
    [[nodiscard]] QString trashRoot() const;
    [[nodiscard]] QString databaseFile() const;
    /// Where the crashed run's group directory is, or would be.
    [[nodiscard]] static QString groupDirectory(const QString& root,
                                                const application::OperationRecord& record);
    /// How many of the photo's files have left their original path.
    [[nodiscard]] int filesGoneFromTheCollection() const;
    /// How many of the photo's files sit in the group's staging directory.
    [[nodiscard]] static int filesIn(const QString& directory);

    std::unique_ptr<TempCollection> collection_;
    std::unique_ptr<QTemporaryDir> workspace_;
    std::unique_ptr<infrastructure::SqliteRepository> repository_;
    std::unique_ptr<FakeTrashAdapter> trash_;
    std::unique_ptr<infrastructure::StagingExecutor> executor_;
    std::unique_ptr<application::OperationController> controller_;
    domain::CollectionId collectionId_;
};

void TestCrashRecovery::init() {
    collection_ = std::make_unique<TempCollection>();
    QVERIFY(collection_->isValid());
    collection_->addJpeg(QStringLiteral("A.JPG"));
    collection_->addRaw(QStringLiteral("A.RAF"));

    // The database lives outside the collection, as it does in production: a file beside the photos
    // would be scanned, marked and moved along with them.
    workspace_ = std::make_unique<QTemporaryDir>();
    QVERIFY(workspace_->isValid());
}

void TestCrashRecovery::cleanup() {
    controller_.reset();
    executor_.reset();
    trash_.reset();
    repository_.reset();
    workspace_.reset();
    collection_.reset();
}

QString TestCrashRecovery::stagingRoot() const {
    return infrastructure::Paths::stagingRootFor(collection_->path());
}

QString TestCrashRecovery::trashRoot() const {
    // A directory on the photos' own filesystem, so the group leaves staging by rename and can
    // still be read back afterwards. The desktop Trash is never involved: the suite must not put
    // anything into the developer's Trash, and could not inspect it if it did.
    return collection_->filePath(QStringLiteral(".trash"));
}

QString TestCrashRecovery::databaseFile() const {
    return QDir(workspace_->path()).absoluteFilePath(QStringLiteral("cullfinch.sqlite"));
}

QString TestCrashRecovery::groupDirectory(const QString& root,
                                          const application::OperationRecord& record) {
    return QDir(root).absoluteFilePath(record.plan.groups.first().stagingDirectoryName);
}

int TestCrashRecovery::filesGoneFromTheCollection() const {
    int gone = 0;
    const QStringList names = photoFiles();
    for (const QString& name : names) {
        if (!QFileInfo::exists(collection_->filePath(name))) {
            ++gone;
        }
    }
    return gone;
}

int TestCrashRecovery::filesIn(const QString& directory) {
    int found = 0;
    const QStringList names = photoFiles();
    for (const QString& name : names) {
        if (QFileInfo::exists(QDir(directory).absoluteFilePath(name))) {
            ++found;
        }
    }
    return found;
}

void TestCrashRecovery::runHelper(CrashPoint point) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("CULLFINCH_CRASH_AT"), testsupport::crashPointToken(point));

    QProcess process;
    process.setProcessEnvironment(environment);
    process.start(QString::fromUtf8(CULLFINCH_CRASH_HELPER),
                  {QStringLiteral("--collection"), collection_->path(), QStringLiteral("--staging"),
                   stagingRoot(), QStringLiteral("--database"), databaseFile(),
                   QStringLiteral("--trash"), trashRoot()});
    QVERIFY2(process.waitForFinished(120000), "the crash helper did not finish");

    const QString diagnostics = QString::fromUtf8(process.readAllStandardError());
    // A helper that died of its own accord proves nothing about recovery, so the exit status is
    // checked before anything on disk is.
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const int expected = point == CrashPoint::None ? 0 : testsupport::kCrashExitCode;
    QVERIFY2(process.exitCode() == expected,
             qPrintable(QStringLiteral("expected exit %1, got %2: %3")
                            .arg(expected)
                            .arg(process.exitCode())
                            .arg(diagnostics)));
}

void TestCrashRecovery::reopen() {
    // Cleared first, so a reopen that bails out leaves a null controller for the caller's guard to
    // catch rather than a stale one from an earlier step.
    controller_.reset();
    repository_ = std::make_unique<infrastructure::SqliteRepository>(databaseFile());
    QString error;
    QVERIFY2(repository_->open(&error), qPrintable(error));

    // findCollection, not ensureCollection: if the crashed run had not recorded the collection, a
    // creating call here would hide that and then find no operations to recover.
    const std::optional<domain::CollectionId> found =
        repository_->findCollection(collection_->path(), &error);
    if (!found.has_value()) {
        QFAIL(qPrintable(error));
    }
    collectionId_ = *found;

    trash_ = std::make_unique<FakeTrashAdapter>();
    executor_ = std::make_unique<infrastructure::StagingExecutor>(*trash_);
    controller_ = std::make_unique<application::OperationController>(*repository_, *executor_);
}

void TestCrashRecovery::crashAndReopen(CrashPoint point, application::OperationRecord* record) {
    runHelper(point);
    reopen();
    if (controller_ == nullptr) {
        return;
    }
    const QList<application::OperationRecord> unfinished = controller_->unfinished(collectionId_);
    QCOMPARE(unfinished.size(), 1);
    QCOMPARE(unfinished.first().plan.groups.size(), 1);
    *record = unfinished.first();
}

void TestCrashRecovery::recoverPutsThePhotoBack(const application::OperationRecord& stored) {
    QString error;
    QVERIFY2(controller_->recover(stored.plan.id, &error), qPrintable(error));
    QCOMPARE(filesGoneFromTheCollection(), 0);
    QCOMPARE(filesIn(groupDirectory(stagingRoot(), stored)), 0);
    QCOMPARE(trash_->callCount(), 0);

    const std::optional<application::OperationRecord> reloaded =
        repository_->loadOperation(stored.plan.id, &error);
    if (!reloaded.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(reloaded->state, domain::OperationState::Planned);
}

void TestCrashRecovery::anUninterruptedRunCompletesAndLeavesNothingUnfinished() {
    // The control case. Without it, a helper that silently failed to start would make every crash
    // case below pass for the wrong reason.
    runHelper(CrashPoint::None);
    reopen();
    QVERIFY(controller_ != nullptr);

    QVERIFY(controller_->unfinished(collectionId_).isEmpty());
    QCOMPARE(filesGoneFromTheCollection(), photoFileCount());

    // The whole group is in Trash as one directory, with the manifest that can put it back.
    const QStringList inTrash = QDir(trashRoot()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(inTrash.size(), 1);
    const QString trashed = QDir(trashRoot()).absoluteFilePath(inTrash.first());
    QCOMPARE(filesIn(trashed), photoFileCount());
    QString error;
    const std::optional<infrastructure::Manifest> manifest =
        infrastructure::StagingExecutor::readManifest(trashed, &error);
    if (!manifest.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(manifest->members.size(), photoFileCount());
}

void TestCrashRecovery::recoversFromACrashBetweenARenameAndItsJournal() {
    application::OperationRecord stored;
    crashAndReopen(CrashPoint::FirstRename, &stored);
    QVERIFY(controller_ != nullptr);
    QCOMPARE(stored.plan.groups.size(), 1);

    // What committed is the intent: every member names the destination it was going to, and none of
    // them claims to have arrived. The filesystem is exactly one rename ahead of that.
    QCOMPARE(stored.state, domain::OperationState::Staging);
    for (const application::OperationMemberRecord& member : stored.members) {
        QCOMPARE(member.lastDurableStep, QStringLiteral("planned"));
        QVERIFY(!member.stagingPath.isEmpty());
    }
    const QString staged = groupDirectory(stagingRoot(), stored);
    QCOMPARE(filesGoneFromTheCollection(), 1);
    QCOMPARE(filesIn(staged), 1);

    // The manifest is written before the first rename precisely so this state is explainable from
    // the staging directory alone, by a process that never saw the journal.
    QString error;
    const std::optional<infrastructure::Manifest> manifest =
        infrastructure::StagingExecutor::readManifest(staged, &error);
    if (!manifest.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(manifest->members.size(), photoFileCount());

    recoverPutsThePhotoBack(stored);
}

void TestCrashRecovery::recoversFromACrashAfterTheWholeGroupReachedStaging() {
    application::OperationRecord stored;
    crashAndReopen(CrashPoint::Staged, &stored);
    QVERIFY(controller_ != nullptr);
    QCOMPARE(stored.plan.groups.size(), 1);

    QCOMPARE(stored.state, domain::OperationState::Staged);
    const QString staged = groupDirectory(stagingRoot(), stored);
    QCOMPARE(filesGoneFromTheCollection(), photoFileCount());
    QCOMPARE(filesIn(staged), photoFileCount());
    QVERIFY(stored.trashPath.isEmpty());

    // Staged is not Trashed: the group is whole, in staging, and must come back.
    recoverPutsThePhotoBack(stored);
    QVERIFY(!QFileInfo::exists(trashRoot()));
}

void TestCrashRecovery::recoversFromACrashAfterTheIntentToTrashWasJournalled() {
    application::OperationRecord stored;
    crashAndReopen(CrashPoint::Trashing, &stored);
    QVERIFY(controller_ != nullptr);
    QCOMPARE(stored.plan.groups.size(), 1);

    // "Trashing" records that Trash is about to be asked, not that it answered. The files prove it:
    // they are still in staging and the Trash directory was never even created.
    QCOMPARE(stored.state, domain::OperationState::Trashing);
    QVERIFY(stored.trashPath.isEmpty());
    const QString staged = groupDirectory(stagingRoot(), stored);
    QCOMPARE(filesIn(staged), photoFileCount());
    QVERIFY(!QFileInfo::exists(trashRoot()));

    // Recovery must read the files, not the state name. Taking "Trashing" as evidence of a Trash
    // would abandon a group that is sitting in staging, intact.
    recoverPutsThePhotoBack(stored);
}

void TestCrashRecovery::reportsAnUncertainOutcomeAfterACrashBetweenTrashAndItsJournal() {
    application::OperationRecord stored;
    crashAndReopen(CrashPoint::Trashed, &stored);
    QVERIFY(controller_ != nullptr);
    QCOMPARE(stored.plan.groups.size(), 1);

    // The one write that would have named the Trash location is the write that never happened, so
    // the journal still reads "Trashing" with no path, and the group is gone from both the
    // collection and staging.
    QCOMPARE(stored.state, domain::OperationState::Trashing);
    QVERIFY(stored.trashPath.isEmpty());
    QCOMPARE(filesGoneFromTheCollection(), photoFileCount());
    QVERIFY(!QFileInfo::exists(groupDirectory(stagingRoot(), stored)));

    // The files are in Trash with their manifest, which is what makes the outcome recoverable by a
    // person even though the journal cannot confirm it.
    const QString trashed = groupDirectory(trashRoot(), stored);
    QCOMPARE(filesIn(trashed), photoFileCount());
    QString error;
    const std::optional<infrastructure::Manifest> manifest =
        infrastructure::StagingExecutor::readManifest(trashed, &error);
    if (!manifest.has_value()) {
        QFAIL(qPrintable(error));
    }
    QVERIFY(manifest->assetId == stored.plan.groups.first().assetId);

    // Recovery refuses to guess. It reports the group as unaccounted for and points at the Trash;
    // what it must never do is decide the photo is gone and move on, or delete anything again.
    QVERIFY(!controller_->recover(stored.plan.id, &error));
    QVERIFY2(error.contains(QStringLiteral("Trash")), qPrintable(error));
    QCOMPARE(trash_->callCount(), 0);
    QCOMPARE(filesIn(trashed), photoFileCount());

    const std::optional<application::OperationRecord> reloaded =
        repository_->loadOperation(stored.plan.id, &error);
    if (!reloaded.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(reloaded->state, domain::OperationState::NeedsRecovery);
}

QTEST_MAIN(TestCrashRecovery)
#include "tst_crash_recovery.moc"
