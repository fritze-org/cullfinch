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
#include <QTest>

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
        executor.executeGroup(record, planning.plan.groups.first());

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
        executor.executeGroup(record, planning.plan.groups.first());

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
        executor.executeGroup(record, planning.plan.groups.first());

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
        executor.executeGroup(record, planning.plan.groups.first());

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
        executor.executeGroup(record, planning.plan.groups.first());
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
    executor.preflight(planning.plan, assets, &error);
    // This deletion policy needs a writable location on the source filesystem,
    // and says so instead of falling back to something else.
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TestStagingExecutor)
#include "tst_staging_executor.moc"
