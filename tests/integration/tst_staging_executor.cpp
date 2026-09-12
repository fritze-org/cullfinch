// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/testsupport/FakeTrashAdapter.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>

#include <tuple>
#include <utility>

#include <unistd.h>

using namespace cullfinch;
using cullfinch::testsupport::FakeTrashAdapter;
using cullfinch::testsupport::TempCollection;

namespace {

QByteArray digestOf(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

domain::PhotoAssetList scan(const TempCollection& collection) {
    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error);
    const domain::StemAssociationResolver resolver;
    return resolver
        .resolve(domain::CollectionId(QStringLiteral("c1")), files,
                 domain::AssociationConfig::defaults(), true)
        .assets;
}

domain::PhotoAssetList markAll(domain::PhotoAssetList assets) {
    for (domain::PhotoAsset& asset : assets) {
        asset.disposition = domain::Disposition::Reject;
    }
    return assets;
}

QString stagingRootOf(const TempCollection& collection) {
    return QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
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

/// The plan for every asset in the collection, all marked for rejection.
domain::PlanningResult planFor(const TempCollection& collection) {
    return domain::OperationPlanner::plan(domain::CollectionId(QStringLiteral("c1")), 1,
                                          stagingRootOf(collection), markAll(scan(collection)));
}

/// The first group of a collection's plan, staged by an executor whose Trash
/// call was made to fail: the files stay in staging for the test to inspect
/// or disturb, and the record it hands back says NeedsRecovery.
struct StagedGroup {
    explicit StagedGroup(const TempCollection& collection)
        : stagingRoot(stagingRootOf(collection)), planning(planFor(collection)), executor(trash) {
        trash.failNextCalls(1);
        staged = executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), {});
    }

    /// Where a file of the first group sits in staging.
    [[nodiscard]] QString stagedPath(const QString& fileName) const {
        return QDir(QDir(stagingRoot)
                        .absoluteFilePath(planning.plan.groups.first().stagingDirectoryName))
            .absoluteFilePath(fileName);
    }

    QString stagingRoot;
    domain::PlanningResult planning;
    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor;
    application::OperationRecord staged;
};

QString stepOf(const application::OperationRecord& record, const QString& fileName) {
    for (const application::OperationMemberRecord& member : record.members) {
        if (QFileInfo(member.sourcePath).fileName() == fileName) {
            return member.lastDurableStep;
        }
    }
    return QStringLiteral("<absent>");
}

} // namespace

class TestStagingExecutor : public QObject {
    Q_OBJECT

private slots:
    void movesTheWholeGroupAndWritesAManifest();
    void preservesOpaqueRawBytes();
    void preflightBlocksAGroupWhoseFileChanged();
    void preflightBlocksAGroupWhoseRawDisappeared();
    void preflightBlocksAGroupWhoseMarkWasCleared();
    void aFailedTrashKeepsTheCompleteGroupForRecovery();
    void neverOverwritesOnRestore();
    void recoveryPutsStagedFilesBack();
    void refusesAnUnwritableStagingRoot();

    void journalsIntentBeforeAndOutcomeAfterEachMove();
    void aRefusedJournalWriteStopsTheGroupBeforeAnythingMoves();
    void recoveryFindsFilesTheJournalNeverConfirmed();
    void recoveryLeavesAStagedFileThatDoesNotMatchTheReview_data();
    void recoveryLeavesAStagedFileThatDoesNotMatchTheReview();
    void recoveryConfirmsATrashOutcomeFromTheManifest();
    void eachGroupRecordsItsOwnTrashPath();
    void recoveryReportsAnUncertainTrashOutcome();
    void preflightBlocksAGroupThatGainedACompanion();
    void preflightIgnoresASidecarThePolicyIgnores();
    void preflightBlocksAGroupOnAnotherFilesystem();

    void readManifestReportsAbsentCorruptAndForeignManifests();
    void recoveryOfAPlanThatNeverStartedChangesNothing();
    void recoveryWorksFromThePlanAloneWhenTheManifestIsGone();
    void recoveryReportsAFileMissingFromAPartlyPresentGroup();
    void recoveryAfterAPartialRunReportsWhatReachedTrash();
    void aFailedRenameMidGroupPutsTheMovedMembersBack();
    void aJournalRefusedMidGroupLeavesTheFilesForRecovery_data();
    void aJournalRefusedMidGroupLeavesTheFilesForRecovery();
    void anUnreadableManifestIsReported();
    void recoveryReportsAFileItCannotPutBack();
};

void TestStagingExecutor::movesTheWholeGroupAndWritesAManifest() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    collection.addRaw(QStringLiteral("A.DNG"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    QCOMPARE(assets.size(), 1);

    const QString stagingRoot =
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRoot, assets);
    QVERIFY(!planning.hasBlockers());

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);

    QString error;
    const domain::PlanningResult verified = executor.preflight(planning.plan, assets, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!verified.hasBlockers());

    application::OperationRecord record;
    record.plan = planning.plan;
    for (const domain::PlannedMember& member : planning.plan.groups.first().members) {
        application::OperationMemberRecord entry;
        entry.memberId = member.memberId;
        entry.assetId = planning.plan.groups.first().assetId;
        entry.sourcePath = member.sourcePath;
        record.members.append(entry);
    }

    const application::OperationRecord result =
        executor.executeGroup(record, planning.plan.groups.first(), {});

    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.state, domain::OperationState::Trashing);
    QCOMPARE(trash.callCount(), 1);

    // All three files left their original locations together.
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.DNG"))));

    // One Trash call for the group directory, not one per file.
    QCOMPARE(trash.trashed().size(), 1);
}

void TestStagingExecutor::preservesOpaqueRawBytes() {
    TempCollection collection;
    const QByteArray payload = QByteArrayLiteral("\x89RAW\x00\x01\x02opaque");
    collection.addJpeg(QStringLiteral("A.JPG"));
    const QString rawPath = collection.addRaw(QStringLiteral("A.RAF"), payload);
    const QByteArray before = digestOf(rawPath);

    // Trash fails, so the staged bytes remain inspectable.
    const StagedGroup group(collection);
    QCOMPARE(group.staged.state, domain::OperationState::NeedsRecovery);
    QString stagedRaw;
    for (const application::OperationMemberRecord& member : group.staged.members) {
        if (member.stagingPath.endsWith(QStringLiteral(".RAF"))) {
            stagedRaw = member.stagingPath;
        }
    }
    QVERIFY(!stagedRaw.isEmpty());
    // The bytes cullfinch never decodes are byte-for-byte unchanged.
    QCOMPARE(digestOf(stagedRaw), before);
}

void TestStagingExecutor::preflightBlocksAGroupWhoseFileChanged() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1,
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging")), assets);

    // Someone edited the JPG after the review screen was shown.
    QTest::qWait(1100);
    collection.addJpeg(QStringLiteral("A.JPG"), QSize(320, 240));

    FakeTrashAdapter trash;
    const infrastructure::StagingExecutor executor(trash);
    QString error;
    const domain::PlanningResult verified =
        executor.preflight(planning.plan, scan(collection), &error);

    QVERIFY2(verified.hasBlockers(), "a changed file must invalidate the reviewed plan");
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::preflightBlocksAGroupWhoseRawDisappeared() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1,
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging")), assets);

    QVERIFY(QFile::remove(collection.filePath(QStringLiteral("A.RAF"))));

    FakeTrashAdapter trash;
    const infrastructure::StagingExecutor executor(trash);
    QString error;
    // A missing previously known RAW blocks the whole group, JPG included.
    const domain::PlanningResult verified =
        executor.preflight(planning.plan, scan(collection), &error);
    QVERIFY(verified.hasBlockers());
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::preflightBlocksAGroupWhoseMarkWasCleared() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1,
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging")), assets);

    // The reject mark is cleared from another window; nothing on disk changes.
    domain::PhotoAssetList current = assets;
    current.first().disposition = domain::Disposition::Neutral;

    FakeTrashAdapter trash;
    const infrastructure::StagingExecutor executor(trash);
    QString error;
    const domain::PlanningResult verified = executor.preflight(planning.plan, current, &error);
    QVERIFY(verified.hasBlockers());
    QVERIFY(verified.blocked.first().reason.contains(QStringLiteral("no longer marked")));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::aFailedTrashKeepsTheCompleteGroupForRecovery() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const StagedGroup group(collection);
    QCOMPARE(group.staged.state, domain::OperationState::NeedsRecovery);
    QVERIFY(!group.staged.error.isEmpty());

    // Nothing was deleted: the complete group waits in staging, and the
    // manifest that a restore needs is there with it.
    QVERIFY(
        QFileInfo::exists(group.stagedPath(infrastructure::StagingExecutor::manifestFileName())));
    QVERIFY(QFileInfo::exists(group.stagedPath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(group.stagedPath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::neverOverwritesOnRestore() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    StagedGroup group(collection);

    // Something recreated the original JPG while the group sat in staging.
    collection.addJpeg(QStringLiteral("A.JPG"));

    const application::OperationRecord recovered = group.executor.recover(group.staged);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY(recovered.error.contains(QStringLiteral("nothing")));

    // The staging copy is retained rather than overwriting the new file.
    QVERIFY(QFileInfo::exists(group.stagedPath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::recoveryPutsStagedFilesBack() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    const QByteArray payload = QByteArrayLiteral("raw-bytes-that-must-survive");
    collection.addRaw(QStringLiteral("A.RAF"), payload);
    const QByteArray before = digestOf(collection.filePath(QStringLiteral("A.RAF")));

    StagedGroup group(collection);
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));

    const application::OperationRecord recovered = group.executor.recover(group.staged);
    QCOMPARE(recovered.state, domain::OperationState::Planned);

    // Both files are back at their exact original paths, byte-for-byte.
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
    QCOMPARE(digestOf(collection.filePath(QStringLiteral("A.RAF"))), before);
}

void TestStagingExecutor::refusesAnUnwritableStagingRoot() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    domain::PlanningResult planning =
        domain::OperationPlanner::plan(domain::CollectionId(QStringLiteral("c1")), 1,
                                       QStringLiteral("/proc/cullfinch-cannot-write-here"), assets);

    FakeTrashAdapter trash;
    const infrastructure::StagingExecutor executor(trash);
    QString error;
    std::ignore = executor.preflight(planning.plan, assets, &error);
    // This deletion policy needs a writable location on the source filesystem,
    // and says so instead of falling back to something else.
    QVERIFY(!error.isEmpty());
}

void TestStagingExecutor::journalsIntentBeforeAndOutcomeAfterEachMove() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);

    // Every journal write is checked against the filesystem *at the moment it
    // happens*: a "staged" step must never be recorded before the file is
    // actually in staging, and a "planned" intent must name the destination.
    QList<application::OperationRecord> snapshots;
    QStringList violations;
    const application::JournalWriter journal =
        [&snapshots, &violations](const application::OperationRecord& record, QString*) {
            snapshots.append(record);
            for (const application::OperationMemberRecord& member : record.members) {
                if (member.stagingPath.isEmpty()) {
                    violations.append(
                        QStringLiteral("no destination recorded for %1").arg(member.sourcePath));
                }
                if (member.lastDurableStep == QLatin1String("staged") &&
                    !QFileInfo::exists(member.stagingPath)) {
                    violations.append(QStringLiteral("%1 journalled as staged before it arrived")
                                          .arg(member.stagingPath));
                }
            }
            return true;
        };

    const application::OperationRecord result =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), journal);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY2(violations.isEmpty(), qPrintable(violations.join(QLatin1String("; "))));

    // Intent, one confirmation per file, Staged, the intent to Trash, and
    // the Trash outcome.
    QCOMPARE(snapshots.size(), 6);
    QCOMPARE(snapshots.at(0).state, domain::OperationState::Staging);
    QCOMPARE(stepOf(snapshots.at(0), QStringLiteral("A.JPG")), QStringLiteral("planned"));
    QCOMPARE(stepOf(snapshots.at(1), QStringLiteral("A.JPG")), QStringLiteral("staged"));
    QCOMPARE(stepOf(snapshots.at(1), QStringLiteral("A.RAF")), QStringLiteral("planned"));
    QCOMPARE(stepOf(snapshots.at(2), QStringLiteral("A.RAF")), QStringLiteral("staged"));
    QCOMPARE(snapshots.at(3).state, domain::OperationState::Staged);
    QCOMPARE(snapshots.at(4).state, domain::OperationState::Trashing);
    QCOMPARE(stepOf(snapshots.at(4), QStringLiteral("A.JPG")), QStringLiteral("staged"));
    QCOMPARE(stepOf(snapshots.at(5), QStringLiteral("A.JPG")), QStringLiteral("trashed"));
    QVERIFY(!snapshots.at(5).trashPath.isEmpty());
    QCOMPARE(trash.callCount(), 1);
}

void TestStagingExecutor::aRefusedJournalWriteStopsTheGroupBeforeAnythingMoves() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);
    const application::JournalWriter refusing = [](const application::OperationRecord&,
                                                   QString* error) {
        *error = QStringLiteral("disk full");
        return false;
    };

    const application::OperationRecord result =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), refusing);
    QCOMPARE(result.state, domain::OperationState::Failed);
    QVERIFY(result.error.contains(QStringLiteral("disk full")));
    // The journal is what makes staged work recoverable, so without it
    // nothing is allowed to move.
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
    QCOMPARE(trash.callCount(), 0);
}

void TestStagingExecutor::recoveryFindsFilesTheJournalNeverConfirmed() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    const QByteArray payload = QByteArrayLiteral("raw-bytes-that-must-survive");
    collection.addRaw(QStringLiteral("A.RAF"), payload);
    const QByteArray before = digestOf(collection.filePath(QStringLiteral("A.RAF")));

    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);
    std::ignore = executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), {});
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));

    // A crash between the renames and their confirmation leaves the journal
    // at "planned" with no staging path at all. That used to be reported as
    // "in neither location" while the files sat intact in staging.
    const application::OperationRecord recovered = executor.recover(recordFor(planning.plan));
    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(recovered.state, domain::OperationState::Planned);
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
    QCOMPARE(digestOf(collection.filePath(QStringLiteral("A.RAF"))), before);
    QCOMPARE(stepOf(recovered, QStringLiteral("A.RAF")), QStringLiteral("restored"));
}

void TestStagingExecutor::recoveryLeavesAStagedFileThatDoesNotMatchTheReview_data() {
    QTest::addColumn<QByteArray>("rewritten");
    QTest::addColumn<bool>("touched");

    // Existence is not identity: a file that no longer matches the review is
    // not put back, whether the rewrite changed its length or only its bytes.
    QTest::newRow("different size") << QByteArrayLiteral("something else entirely") << false;
    // Rewritten at the same length, the size alone would call this the
    // reviewed file. The modification time is part of the identity.
    QTest::newRow("same size, later modification") << QByteArrayLiteral("modified") << true;
}

void TestStagingExecutor::recoveryLeavesAStagedFileThatDoesNotMatchTheReview() {
    QFETCH(const QByteArray, rewritten);
    QFETCH(const bool, touched);

    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"), QByteArrayLiteral("original"));
    StagedGroup group(collection);
    QCOMPARE(group.staged.state, domain::OperationState::NeedsRecovery);

    // Something rewrote the staged RAW while it waited.
    const QString stagedRaw = group.stagedPath(QStringLiteral("A.RAF"));
    {
        QFile file(stagedRaw);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(rewritten), rewritten.size());
        // The bytes have to reach the file before its modification time is
        // stamped. setFileTime() does not flush, so without this the buffered
        // write lands on close() -- after the stamp -- and the kernel puts the
        // modification time back to now, which is the very timestamp the
        // review recorded. The rewrite then still looks like the reviewed
        // file and recovery restores it.
        QVERIFY(file.flush());
        if (touched) {
            QVERIFY(file.setFileTime(QDateTime::currentDateTimeUtc().addSecs(120),
                                     QFileDevice::FileModificationTime));
        }
    }
    QCOMPARE(QFileInfo(stagedRaw).size(), rewritten.size());

    const application::OperationRecord recovered = group.executor.recover(group.staged);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(recovered.error.contains(QStringLiteral("A.RAF")), qPrintable(recovered.error));
    QVERIFY(QFileInfo::exists(stagedRaw));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
    // The JPG did match and is back; the operation still needs a person.
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::recoveryConfirmsATrashOutcomeFromTheManifest() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);

    // Every journal write lands except the one after Trash returned: the
    // write a crash at the worst moment loses, and the one the executor
    // reports rather than swallows.
    QList<application::OperationRecord> snapshots;
    const application::JournalWriter journal =
        [&snapshots](const application::OperationRecord& record, QString* error) {
            if (stepOf(record, QStringLiteral("A.JPG")) == QStringLiteral("trashed")) {
                if (error != nullptr) {
                    *error = QStringLiteral("journal refused after Trash");
                }
                return false;
            }
            snapshots.append(record);
            return true;
        };
    const application::OperationRecord done =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), journal);
    QCOMPARE(done.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(done.error.contains(QStringLiteral("reached Trash")), qPrintable(done.error));
    QVERIFY(!done.trashPath.isEmpty());
    QCOMPARE(stepOf(done, QStringLiteral("A.JPG")), QStringLiteral("trashed"));

    // What the journal holds is the record as written just before Trash was
    // asked: no location yet, members staged. From that alone nothing can be
    // confirmed, and recovery says so instead of guessing.
    const application::OperationRecord beforeTrash = snapshots.last();
    QCOMPARE(beforeTrash.state, domain::OperationState::Trashing);
    QVERIFY(beforeTrash.trashPath.isEmpty());
    QCOMPARE(stepOf(beforeTrash, QStringLiteral("A.JPG")), QStringLiteral("staged"));
    application::OperationRecord recovered = executor.recover(beforeTrash);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(recovered.error.contains(QStringLiteral("Trash")), qPrintable(recovered.error));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));

    // The record the executor handed back knows the members reached Trash,
    // and recovery believes that journal step.
    recovered = executor.recover(done);
    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(recovered.state, domain::OperationState::Completed);

    // A record that knows the platform's location but whose members are one
    // step behind is confirmed from the manifest inside that location.
    application::OperationRecord locationOnly = beforeTrash;
    locationOnly.trashPath = done.trashPath;
    recovered = executor.recover(locationOnly);
    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(recovered.state, domain::OperationState::Completed);
    QCOMPARE(stepOf(recovered, QStringLiteral("A.RAF")), QStringLiteral("trashed"));
    QCOMPARE(trash.callCount(), 1);
}

void TestStagingExecutor::eachGroupRecordsItsOwnTrashPath() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addJpeg(QStringLiteral("B.JPG"));

    const domain::PlanningResult planning = planFor(collection);
    QCOMPARE(planning.plan.groups.size(), 2);

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);

    QList<application::OperationRecord> snapshots;
    const application::JournalWriter journal =
        [&snapshots](const application::OperationRecord& record, QString*) {
            snapshots.append(record);
            return true;
        };
    const application::OperationRecord first =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), journal);
    QCOMPARE(first.state, domain::OperationState::Trashing);
    QVERIFY(!first.trashPath.isEmpty());

    // The second group starts from the first group's record, as the
    // controller runs them. Its "Trash is about to be asked" entry must not
    // still carry the first group's location: recovery from that entry would
    // check the wrong manifest and confirm nothing.
    snapshots.clear();
    const application::OperationRecord second =
        executor.executeGroup(first, planning.plan.groups.last(), journal);
    QCOMPARE(second.state, domain::OperationState::Trashing);
    QVERIFY(!second.trashPath.isEmpty());
    QVERIFY(second.trashPath != first.trashPath);

    bool sawTrashing = false;
    for (const application::OperationRecord& snapshot : std::as_const(snapshots)) {
        if (snapshot.state == domain::OperationState::Trashing &&
            stepOf(snapshot, QStringLiteral("B.JPG")) == QStringLiteral("staged")) {
            sawTrashing = true;
            QVERIFY(snapshot.trashPath.isEmpty());
        }
    }
    QVERIFY(sawTrashing);

    const application::OperationRecord recovered = executor.recover(second);
    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(recovered.state, domain::OperationState::Completed);
}

void TestStagingExecutor::recoveryReportsAnUncertainTrashOutcome() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    trash.setReportsPath(false); // The platform is allowed to say nothing.
    infrastructure::StagingExecutor executor(trash);

    // The record as journalled just before Trash was asked.
    QList<application::OperationRecord> snapshots;
    const application::JournalWriter journal =
        [&snapshots](const application::OperationRecord& record, QString*) {
            snapshots.append(record);
            return true;
        };
    std::ignore =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), journal);
    QVERIFY(snapshots.size() >= 2);
    const application::OperationRecord beforeTrash = snapshots.at(snapshots.size() - 2);
    QCOMPARE(beforeTrash.state, domain::OperationState::Trashing);
    QVERIFY(beforeTrash.trashPath.isEmpty());

    // The group is gone from both places and nothing can confirm where to:
    // that is recorded as uncertain and handed to a person. It is never
    // treated as "lost", and never as licence to delete anything again.
    const application::OperationRecord recovered = executor.recover(beforeTrash);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(recovered.error.contains(QStringLiteral("Trash")), qPrintable(recovered.error));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QCOMPARE(trash.callCount(), 1);
}

void TestStagingExecutor::preflightBlocksAGroupThatGainedACompanion() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

    // A second RAW arrived after the review, before any rescan. Moving the
    // rest of the group would orphan it, so preflight re-enumerates.
    collection.addRaw(QStringLiteral("A.DNG"));

    FakeTrashAdapter trash;
    const infrastructure::StagingExecutor executor(trash);
    QString error;
    const domain::PlanningResult verified = executor.preflight(planning.plan, assets, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(verified.hasBlockers());
    QVERIFY(verified.blocked.first().reason.contains(QStringLiteral("A.DNG")));
}

void TestStagingExecutor::preflightIgnoresASidecarThePolicyIgnores() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

    // Sidecars are switched off by default, so an XMP beside the photo is
    // not part of it -- the scan ignores it and so must preflight, or the
    // group could never be moved.
    collection.addFile(QStringLiteral("A.xmp"), QByteArrayLiteral("<xmp/>"));

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);
    executor.setAssociationConfig(domain::AssociationConfig::defaults());
    QString error;
    domain::PlanningResult verified = executor.preflight(planning.plan, assets, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(!verified.hasBlockers(), qPrintable(verified.blocked.value(0).reason));

    // An unrecognised same-stem file is a different matter: the scan would
    // have flagged it, and moving the rest of the group would orphan it.
    collection.addFile(QStringLiteral("A.txt"), QByteArrayLiteral("notes"));
    verified = executor.preflight(planning.plan, assets, &error);
    QVERIFY(verified.hasBlockers());
    QVERIFY(verified.blocked.first().reason.contains(QStringLiteral("A.txt")));
}

void TestStagingExecutor::preflightBlocksAGroupOnAnotherFilesystem() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    // Find a writable directory on a different filesystem from the photos.
    QString elsewhere;
    for (const QString& candidate :
         {QStringLiteral("/dev/shm"), QStringLiteral("/run/user"), QStringLiteral("/var/tmp")}) {
        if (QFileInfo(candidate).isWritable() &&
            QStorageInfo(candidate).device() != QStorageInfo(collection.path()).device()) {
            elsewhere = candidate;
            break;
        }
    }
    if (elsewhere.isEmpty()) {
        QSKIP("no second writable filesystem is available on this machine");
    }
    QTemporaryDir stagingRoot(QDir(elsewhere).absoluteFilePath(QStringLiteral("cullfinch-XXXXXX")));
    QVERIFY(stagingRoot.isValid());

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRoot.path(), assets);

    FakeTrashAdapter trash;
    const infrastructure::StagingExecutor executor(trash);
    QString error;
    const domain::PlanningResult verified = executor.preflight(planning.plan, assets, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // Blocked before anything moves, not at member N with the JPG already gone.
    QVERIFY(verified.hasBlockers());
    QVERIFY(verified.blocked.first().reason.contains(QStringLiteral("filesystem")));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::readManifestReportsAbsentCorruptAndForeignManifests() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(directory.path())
                             .absoluteFilePath(infrastructure::StagingExecutor::manifestFileName());

    QString error;
    QVERIFY(!infrastructure::StagingExecutor::readManifest(directory.path(), &error).has_value());
    QVERIFY2(error.contains(QStringLiteral("no recovery manifest")), qPrintable(error));

    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("{ not json"));
    }
    error.clear();
    QVERIFY(!infrastructure::StagingExecutor::readManifest(directory.path(), &error).has_value());
    QVERIFY2(error.contains(QStringLiteral("not readable")), qPrintable(error));

    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArrayLiteral("{\"version\": 99, \"members\": []}"));
    }
    error.clear();
    // A manifest from a newer or unknown writer is reported, never guessed at.
    QVERIFY(!infrastructure::StagingExecutor::readManifest(directory.path(), &error).has_value());
    QVERIFY2(error.contains(QStringLiteral("unknown version")), qPrintable(error));
}

void TestStagingExecutor::recoveryOfAPlanThatNeverStartedChangesNothing() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);
    // A crash right after the plan was recorded: every file is still at its
    // source and the plan may simply be retried.
    const application::OperationRecord recovered = executor.recover(recordFor(planning.plan));
    QCOMPARE(recovered.state, domain::OperationState::Planned);
    QVERIFY(recovered.error.isEmpty());
    QCOMPARE(stepOf(recovered, QStringLiteral("A.JPG")), QStringLiteral("planned"));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::recoveryWorksFromThePlanAloneWhenTheManifestIsGone() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    const domain::PlanningResult planning = planFor(collection);

    FakeTrashAdapter trash;
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);
    std::ignore = executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), {});

    // Neither the journal (still at "planned") nor the manifest can say where
    // the files went; the plan's own staging layout still can.
    const QString directory =
        QDir(stagingRootOf(collection))
            .absoluteFilePath(planning.plan.groups.first().stagingDirectoryName);
    QVERIFY(QFile::remove(
        QDir(directory).absoluteFilePath(infrastructure::StagingExecutor::manifestFileName())));

    const application::OperationRecord recovered = executor.recover(recordFor(planning.plan));
    QCOMPARE(recovered.state, domain::OperationState::Planned);
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::recoveryReportsAFileMissingFromAPartlyPresentGroup() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    StagedGroup group(collection);

    // The RAW vanished from staging while the JPG is still there. That is
    // not a Trash outcome and not something to assume completed.
    QVERIFY(QFile::remove(group.stagedPath(QStringLiteral("A.RAF"))));

    const application::OperationRecord recovered = group.executor.recover(group.staged);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(recovered.error.contains(QStringLiteral("A.RAF")), qPrintable(recovered.error));
    QVERIFY(!recovered.error.contains(QStringLiteral("Trash")));
    // What could be put back, was.
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::recoveryAfterAPartialRunReportsWhatReachedTrash() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    collection.addJpeg(QStringLiteral("B.JPG"));
    const domain::PlanningResult planning = planFor(collection);
    QCOMPARE(planning.plan.groups.size(), 2);

    // The first group reaches Trash; Trash refuses the second.
    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);
    application::OperationRecord record =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.at(0), {});
    QCOMPARE(record.state, domain::OperationState::Trashing);
    trash.failNextCalls(1);
    record = executor.executeGroup(record, planning.plan.groups.at(1), {});
    QCOMPARE(record.state, domain::OperationState::NeedsRecovery);

    // Recovery puts the second group back and says plainly that the first
    // is already in Trash: the plan cannot be retried as it stands.
    const application::OperationRecord recovered = executor.recover(record);
    QCOMPARE(recovered.state, domain::OperationState::Failed);
    QVERIFY2(recovered.error.contains(QStringLiteral("1 photo")), qPrintable(recovered.error));
    const QString first = planning.plan.groups.at(0).members.first().fileName;
    const QString second = planning.plan.groups.at(1).members.first().fileName;
    QVERIFY(!QFileInfo::exists(collection.filePath(first)));
    QVERIFY(QFileInfo::exists(collection.filePath(second)));
    QCOMPARE(stepOf(recovered, first), QStringLiteral("trashed"));
    QCOMPARE(stepOf(recovered, second), QStringLiteral("restored"));
}

void TestStagingExecutor::aFailedRenameMidGroupPutsTheMovedMembersBack() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    const domain::PlanningResult planning = planFor(collection);
    const domain::PlannedGroup& group = planning.plan.groups.first();
    QCOMPARE(group.members.size(), 2);

    // The last member disappears after preflight, so its rename fails after
    // the first member has already moved.
    const domain::PlannedMember& last = group.members.last();
    const domain::PlannedMember& first = group.members.first();
    QVERIFY(QFile::remove(last.sourcePath));

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);
    const application::OperationRecord result =
        executor.executeGroup(recordFor(planning.plan), group, {});
    QCOMPARE(result.state, domain::OperationState::Failed);
    QVERIFY2(result.error.contains(last.fileName), qPrintable(result.error));
    // The member that had moved is back where it was, and says so.
    QVERIFY(QFileInfo::exists(first.sourcePath));
    QCOMPARE(stepOf(result, first.fileName), QStringLiteral("restored"));
    QCOMPARE(trash.callCount(), 0);
}

void TestStagingExecutor::aJournalRefusedMidGroupLeavesTheFilesForRecovery_data() {
    QTest::addColumn<int>("failingWrite");
    QTest::addColumn<QString>("expectedMention");
    // Writes for a two-member group: intent, JPG staged, RAF staged, Staged,
    // Trashing. The first is covered elsewhere (nothing has moved yet).
    QTest::newRow("after the first move") << 2 << QStringLiteral("already been moved");
    QTest::newRow("after the last move") << 3 << QStringLiteral("already been moved");
    QTest::newRow("at Staged") << 4 << QStringLiteral("journal");
    QTest::newRow("at Trashing") << 5 << QStringLiteral("journal");
}

void TestStagingExecutor::aJournalRefusedMidGroupLeavesTheFilesForRecovery() {
    QFETCH(const int, failingWrite);
    QFETCH(const QString, expectedMention);

    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    const domain::PlanningResult planning = planFor(collection);

    int writes = 0;
    const application::JournalWriter flaky =
        [&writes, failingWrite](const application::OperationRecord&, QString* error) {
            if (++writes == failingWrite) {
                *error = QStringLiteral("disk full");
                return false;
            }
            return true;
        };

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);
    const application::OperationRecord result =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), flaky);

    // Once a file has moved, a journal that cannot be written is a recovery
    // task, not a failure to shrug off -- and never a reason to call Trash.
    QCOMPARE(result.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(result.error.contains(expectedMention), qPrintable(result.error));
    QCOMPARE(trash.callCount(), 0);
    const QString directory =
        QDir(stagingRootOf(collection))
            .absoluteFilePath(planning.plan.groups.first().stagingDirectoryName);
    QVERIFY(QFileInfo::exists(QDir(directory).absoluteFilePath(QStringLiteral("A.JPG"))) ||
            QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));

    // And recovery from that record puts everything back.
    const application::OperationRecord recovered = executor.recover(result);
    QCOMPARE(recovered.state, domain::OperationState::Planned);
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::anUnreadableManifestIsReported() {
    if (::geteuid() == 0) {
        QSKIP("root ignores file permissions, so an unreadable file cannot be staged");
    }
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = QDir(directory.path())
                             .absoluteFilePath(infrastructure::StagingExecutor::manifestFileName());
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArrayLiteral("{}"));
    }
    QVERIFY(QFile::setPermissions(path, QFileDevice::Permissions{}));

    QString error;
    QVERIFY(!infrastructure::StagingExecutor::readManifest(directory.path(), &error).has_value());
    QVERIFY2(error.contains(QStringLiteral("could not be read")), qPrintable(error));
    QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
}

void TestStagingExecutor::recoveryReportsAFileItCannotPutBack() {
    if (::geteuid() == 0) {
        QSKIP("root ignores directory permissions, so the rename back cannot be made to fail");
    }
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));
    StagedGroup group(collection);

    // The photo directory became read-only while the group sat in staging.
    const QFileDevice::Permissions original = QFile::permissions(collection.path());
    QVERIFY(
        QFile::setPermissions(collection.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const application::OperationRecord recovered = group.executor.recover(group.staged);
    QVERIFY(QFile::setPermissions(collection.path(), original));

    // Nothing was lost: the files stay in staging, and the record says why.
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY2(recovered.error.contains(QStringLiteral("failed")), qPrintable(recovered.error));
    QVERIFY(QFileInfo::exists(group.stagedPath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(group.stagedPath(QStringLiteral("A.RAF"))));
}

QTEST_MAIN(TestStagingExecutor)
#include "tst_staging_executor.moc"
