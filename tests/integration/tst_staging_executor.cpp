// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/testsupport/FakeTrashAdapter.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QTest>

#include <tuple>

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
    void aFailedTrashKeepsTheCompleteGroupForRecovery();
    void neverOverwritesOnRestore();
    void recoveryPutsStagedFilesBack();
    void refusesAnUnwritableStagingRoot();

    void journalsIntentBeforeAndOutcomeAfterEachMove();
    void aRefusedJournalWriteStopsTheGroupBeforeAnythingMoves();
    void recoveryFindsFilesTheJournalNeverConfirmed();
    void recoveryLeavesAStagedFileThatDoesNotMatchTheReview();
    void recoveryConfirmsATrashOutcomeFromTheManifest();
    void recoveryReportsAnUncertainTrashOutcome();
    void preflightBlocksAGroupThatGainedACompanion();
    void preflightBlocksAGroupOnAnotherFilesystem();
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

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const QString stagingRoot =
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRoot, assets);

    FakeTrashAdapter trash;
    // Fail the Trash step, so the staged bytes remain inspectable.
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);

    application::OperationRecord record;
    record.plan = planning.plan;
    for (const domain::PlannedMember& member : planning.plan.groups.first().members) {
        record.members.append(application::OperationMemberRecord{
            member.memberId, planning.plan.groups.first().assetId, member.sourcePath, {}, {}, {}});
    }
    const application::OperationRecord result =
        executor.executeGroup(record, planning.plan.groups.first(), {});

    QCOMPARE(result.state, domain::OperationState::NeedsRecovery);
    QString stagedRaw;
    for (const application::OperationMemberRecord& member : result.members) {
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

void TestStagingExecutor::aFailedTrashKeepsTheCompleteGroupForRecovery() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const QString stagingRoot =
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRoot, assets);

    FakeTrashAdapter trash;
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);

    application::OperationRecord record;
    record.plan = planning.plan;
    for (const domain::PlannedMember& member : planning.plan.groups.first().members) {
        record.members.append(application::OperationMemberRecord{
            member.memberId, planning.plan.groups.first().assetId, member.sourcePath, {}, {}, {}});
    }
    const application::OperationRecord staged =
        executor.executeGroup(record, planning.plan.groups.first(), {});

    QCOMPARE(staged.state, domain::OperationState::NeedsRecovery);
    QVERIFY(!staged.error.isEmpty());

    // Nothing was deleted: the complete group waits in staging, and the
    // manifest that a restore needs is there with it.
    const QString directory =
        QDir(stagingRoot).absoluteFilePath(planning.plan.groups.first().stagingDirectoryName);
    QVERIFY(QFileInfo::exists(
        QDir(directory).absoluteFilePath(infrastructure::StagingExecutor::manifestFileName())));
    QVERIFY(QFileInfo::exists(QDir(directory).absoluteFilePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(QDir(directory).absoluteFilePath(QStringLiteral("A.RAF"))));
}

void TestStagingExecutor::neverOverwritesOnRestore() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const QString stagingRoot =
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRoot, assets);

    FakeTrashAdapter trash;
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);

    application::OperationRecord record;
    record.plan = planning.plan;
    for (const domain::PlannedMember& member : planning.plan.groups.first().members) {
        record.members.append(application::OperationMemberRecord{
            member.memberId, planning.plan.groups.first().assetId, member.sourcePath, {}, {}, {}});
    }
    application::OperationRecord staged =
        executor.executeGroup(record, planning.plan.groups.first(), {});

    // Something recreated the original JPG while the group sat in staging.
    collection.addJpeg(QStringLiteral("A.JPG"));

    const application::OperationRecord recovered = executor.recover(staged);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY(recovered.error.contains(QStringLiteral("nothing")));

    // The staging copy is retained rather than overwriting the new file.
    const QString directory =
        QDir(stagingRoot).absoluteFilePath(planning.plan.groups.first().stagingDirectoryName);
    QVERIFY(QFileInfo::exists(QDir(directory).absoluteFilePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::recoveryPutsStagedFilesBack() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    const QByteArray payload = QByteArrayLiteral("raw-bytes-that-must-survive");
    collection.addRaw(QStringLiteral("A.RAF"), payload);
    const QByteArray before = digestOf(collection.filePath(QStringLiteral("A.RAF")));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const QString stagingRoot =
        QDir(collection.path()).absoluteFilePath(QStringLiteral(".cullfinch-staging"));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRoot, assets);

    FakeTrashAdapter trash;
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);

    application::OperationRecord record;
    record.plan = planning.plan;
    for (const domain::PlannedMember& member : planning.plan.groups.first().members) {
        record.members.append(application::OperationMemberRecord{
            member.memberId, planning.plan.groups.first().assetId, member.sourcePath, {}, {}, {}});
    }
    const application::OperationRecord staged =
        executor.executeGroup(record, planning.plan.groups.first(), {});
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));

    const application::OperationRecord recovered = executor.recover(staged);
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

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

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

    // Intent, one confirmation per file, Staged, then the intent to Trash.
    QCOMPARE(snapshots.size(), 5);
    QCOMPARE(snapshots.at(0).state, domain::OperationState::Staging);
    QCOMPARE(stepOf(snapshots.at(0), QStringLiteral("A.JPG")), QStringLiteral("planned"));
    QCOMPARE(stepOf(snapshots.at(1), QStringLiteral("A.JPG")), QStringLiteral("staged"));
    QCOMPARE(stepOf(snapshots.at(1), QStringLiteral("A.RAF")), QStringLiteral("planned"));
    QCOMPARE(stepOf(snapshots.at(2), QStringLiteral("A.RAF")), QStringLiteral("staged"));
    QCOMPARE(snapshots.at(3).state, domain::OperationState::Staged);
    QCOMPARE(snapshots.at(4).state, domain::OperationState::Trashing);
    QCOMPARE(trash.callCount(), 1);
}

void TestStagingExecutor::aRefusedJournalWriteStopsTheGroupBeforeAnythingMoves() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

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

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

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

void TestStagingExecutor::recoveryLeavesAStagedFileThatDoesNotMatchTheReview() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"), QByteArrayLiteral("original"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

    FakeTrashAdapter trash;
    trash.failNextCalls(1);
    infrastructure::StagingExecutor executor(trash);
    const application::OperationRecord staged =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), {});
    QCOMPARE(staged.state, domain::OperationState::NeedsRecovery);

    // Something rewrote the staged RAW while it waited. Existence is not
    // identity: a file that no longer matches the review is not put back.
    const QString directory =
        QDir(stagingRootOf(collection))
            .absoluteFilePath(planning.plan.groups.first().stagingDirectoryName);
    const QString stagedRaw = QDir(directory).absoluteFilePath(QStringLiteral("A.RAF"));
    {
        QFile file(stagedRaw);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QByteArrayLiteral("something else entirely"));
    }

    const application::OperationRecord recovered = executor.recover(staged);
    QCOMPARE(recovered.state, domain::OperationState::NeedsRecovery);
    QVERIFY(recovered.error.contains(QStringLiteral("A.RAF")));
    QVERIFY(QFileInfo::exists(stagedRaw));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.RAF"))));
    // The JPG did match and is back; the operation still needs a person.
    QVERIFY(QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::recoveryConfirmsATrashOutcomeFromTheManifest() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

    FakeTrashAdapter trash;
    infrastructure::StagingExecutor executor(trash);

    // Keep the last record written *before* Trash was asked: that is what the
    // journal holds after a crash between the Trash call and its commit.
    application::OperationRecord beforeTrash;
    const application::JournalWriter journal =
        [&beforeTrash](const application::OperationRecord& record, QString*) {
            beforeTrash = record;
            return true;
        };
    const application::OperationRecord done =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), journal);
    QCOMPARE(done.state, domain::OperationState::Trashing);
    QCOMPARE(beforeTrash.state, domain::OperationState::Trashing);
    QCOMPARE(stepOf(beforeTrash, QStringLiteral("A.JPG")), QStringLiteral("staged"));

    // The platform reported where the group went, and its manifest is there.
    beforeTrash.trashPath = done.trashPath;
    QVERIFY(!beforeTrash.trashPath.isEmpty());
    const auto recovered = executor.recover(beforeTrash);
    QVERIFY2(recovered.error.isEmpty(), qPrintable(recovered.error));
    QCOMPARE(recovered.state, domain::OperationState::Completed);
    QCOMPARE(stepOf(recovered, QStringLiteral("A.RAF")), QStringLiteral("trashed"));
    QVERIFY(!QFileInfo::exists(collection.filePath(QStringLiteral("A.JPG"))));
}

void TestStagingExecutor::recoveryReportsAnUncertainTrashOutcome() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("A.RAF"));

    const domain::PhotoAssetList assets = markAll(scan(collection));
    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        domain::CollectionId(QStringLiteral("c1")), 1, stagingRootOf(collection), assets);

    FakeTrashAdapter trash;
    trash.setReportsPath(false); // The platform is allowed to say nothing.
    infrastructure::StagingExecutor executor(trash);

    application::OperationRecord beforeTrash;
    const application::JournalWriter journal =
        [&beforeTrash](const application::OperationRecord& record, QString*) {
            beforeTrash = record;
            return true;
        };
    std::ignore =
        executor.executeGroup(recordFor(planning.plan), planning.plan.groups.first(), journal);
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

QTEST_MAIN(TestStagingExecutor)
#include "tst_staging_executor.moc"
