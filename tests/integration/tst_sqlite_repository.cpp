// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/SqliteRepository.h>
#include <cullfinch/testsupport/AssetBuilder.h>

#include <QSqlDatabase>
#include <QTemporaryDir>
#include <QTest>

using namespace cullfinch;
using cullfinch::testsupport::AssetBuilder;

class TestSqliteRepository : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void createsItsSchemaOnFirstOpen();
    void reopeningIsIdempotent();
    void reusesTheCollectionIdentityForTheSameRoot();
    void storesAndReloadsAssetsWithTheirMembers();
    void keepsMarksWhenMembershipIsUnchanged();
    void invalidatesMarksWhenMembershipChanges();
    void marksAnAssetStaleWhenAMemberDisappears();
    void refusesAMarkChangeAgainstAStaleRevision();
    void roundTripsASessionDraft();
    void listsOnlyResumableSessions();
    void roundTripsAnOperationJournal();

private:
    QTemporaryDir directory_;
    std::unique_ptr<infrastructure::SqliteRepository> repository_;
    QString databaseFile_;
};

void TestSqliteRepository::initTestCase() {
    QVERIFY2(QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")),
             "the deployed package must provide the SQLite driver plugin");
}

void TestSqliteRepository::init() {
    QVERIFY(directory_.isValid());
    databaseFile_ = directory_.filePath(
        QStringLiteral("cullfinch-%1.sqlite").arg(QTest::currentTestFunction()));
    repository_ = std::make_unique<infrastructure::SqliteRepository>(databaseFile_);
    QString error;
    QVERIFY2(repository_->open(&error), qPrintable(error));
}

void TestSqliteRepository::cleanup() {
    repository_.reset();
}

void TestSqliteRepository::createsItsSchemaOnFirstOpen() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos/a"), false, &error);
    QVERIFY2(id.has_value(), qPrintable(error));
    QVERIFY(id->isValid());
    QVERIFY(repository_->collectionRevision(*id, &error) > 0);
}

void TestSqliteRepository::reopeningIsIdempotent() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos/a"), false, &error);
    repository_->close();

    infrastructure::SqliteRepository reopened(databaseFile_);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    const auto again = reopened.ensureCollection(QStringLiteral("/photos/a"), false, &error);
    QVERIFY(again.has_value());
    QCOMPARE(again->toString(), id->toString());
}

void TestSqliteRepository::reusesTheCollectionIdentityForTheSameRoot() {
    QString error;
    const auto first = repository_->ensureCollection(QStringLiteral("/photos/a"), false, &error);
    const auto second = repository_->ensureCollection(QStringLiteral("/photos/a"), true, &error);
    const auto other = repository_->ensureCollection(QStringLiteral("/photos/b"), false, &error);

    QCOMPARE(first->toString(), second->toString());
    QVERIFY(other->toString() != first->toString());
}

void TestSqliteRepository::storesAndReloadsAssetsWithTheirMembers() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(4);

    domain::PhotoAssetList merged;
    quint64 revision = 0;
    QVERIFY2(repository_->reconcileAssets(*id, assets, &merged, &revision, &error),
             qPrintable(error));
    QCOMPARE(merged.size(), 4);

    const domain::PhotoAssetList reloaded = repository_->loadAssets(*id, &error);
    QCOMPARE(reloaded.size(), 4);
    for (const domain::PhotoAsset& asset : reloaded) {
        QCOMPARE(asset.members.size(), 2);
        QCOMPARE(asset.pairingState, domain::PairingState::Resolved);
        QVERIFY(asset.preview() != nullptr);
        QCOMPARE(asset.preview()->role, domain::MemberRole::Jpeg);
        QVERIFY(asset.membershipRevision > 0 || asset.membershipRevision == 1);
    }
}

void TestSqliteRepository::keepsMarksWhenMembershipIsUnchanged() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(3);

    quint64 revision = 0;
    repository_->reconcileAssets(*id, assets, nullptr, &revision, &error);
    QVERIFY2(
        repository_->applyDispositions(*id, revision, {assets.at(1).id}, {}, &revision, &error),
        qPrintable(error));

    // A rescan that finds exactly the same files keeps the mark.
    domain::PhotoAssetList merged;
    QVERIFY(repository_->reconcileAssets(*id, assets, &merged, &revision, &error));

    for (const domain::PhotoAsset& asset : merged) {
        const bool expected = asset.id == assets.at(1).id;
        QCOMPARE(asset.disposition == domain::Disposition::Reject, expected);
    }
}

void TestSqliteRepository::invalidatesMarksWhenMembershipChanges() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);

    quint64 revision = 0;
    repository_->reconcileAssets(*id, assets, nullptr, &revision, &error);
    repository_->applyDispositions(*id, revision, {assets.at(0).id}, {}, &revision, &error);

    // The JPG was replaced by a different file: the prior decision no longer
    // describes what is on disk, so it is not inherited.
    assets[0].membershipRevision = 424242;
    assets[0].members[0].absolutePath = QStringLiteral("/photos/replacement.JPG");

    domain::PhotoAssetList merged;
    QVERIFY(repository_->reconcileAssets(*id, assets, &merged, &revision, &error));
    for (const domain::PhotoAsset& asset : merged) {
        if (asset.id == assets.at(0).id) {
            QCOMPARE(asset.disposition, domain::Disposition::Neutral);
        }
    }
}

void TestSqliteRepository::marksAnAssetStaleWhenAMemberDisappears() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    const domain::PhotoAsset paired =
        AssetBuilder(QStringLiteral("A")).withJpeg().withRaw().build();

    quint64 revision = 0;
    repository_->reconcileAssets(*id, {paired}, nullptr, &revision, &error);

    // The RAW vanished. The expected member is retained and the group is stale;
    // it is never quietly reclassified as JPG-only.
    domain::PhotoAsset jpegOnly = paired;
    jpegOnly.members.removeLast();
    jpegOnly.pairingState = domain::PairingState::JpegOnly;

    domain::PhotoAssetList merged;
    QVERIFY(repository_->reconcileAssets(*id, {jpegOnly}, &merged, &revision, &error));
    QCOMPARE(merged.size(), 1);
    QCOMPARE(merged.first().pairingState, domain::PairingState::Stale);
    QCOMPARE(merged.first().members.size(), 2);
    QVERIFY(!merged.first().isOperable());
    QVERIFY(!merged.first().diagnostics.isEmpty());
}

void TestSqliteRepository::refusesAMarkChangeAgainstAStaleRevision() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);

    quint64 revision = 0;
    repository_->reconcileAssets(*id, assets, nullptr, &revision, &error);

    quint64 ignored = 0;
    QVERIFY2(!repository_->applyDispositions(*id, revision - 1, {assets.first().id}, {}, &ignored,
                                             &error),
             "a mark change must be guarded by the collection revision");
    QVERIFY(!error.isEmpty());

    for (const domain::PhotoAsset& asset : repository_->loadAssets(*id, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
}

void TestSqliteRepository::roundTripsASessionDraft() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(3);

    application::StoredSession session;
    session.id = domain::SessionId::generate();
    session.collectionId = *id;
    session.snapshot = domain::SelectionSnapshot::freeze(*id, 5, assets);
    session.draft.flowId = QStringLiteral("versus-tree");
    session.draft.schemaVersion = 1;
    session.draft.revision = 9;
    session.draft.payload.insert(QStringLiteral("bracketSize"), 4);
    session.draftRejected = {assets.first().id};
    session.lifecycle = application::SessionLifecycle::Paused;

    QVERIFY2(repository_->saveSession(session, &error), qPrintable(error));

    const auto reloaded = repository_->loadSession(session.id, &error);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->draft.flowId, session.draft.flowId);
    QCOMPARE(reloaded->draft.revision, 9U);
    QCOMPARE(reloaded->draft.payload.value(QStringLiteral("bracketSize")).toInt(), 4);
    QCOMPARE(reloaded->snapshot.orderedAssetIds, session.snapshot.orderedAssetIds);
    QCOMPARE(reloaded->snapshot.collectionRevision, 5U);
    QCOMPARE(reloaded->draftRejected, session.draftRejected);
    QCOMPARE(reloaded->lifecycle, application::SessionLifecycle::Paused);
}

void TestSqliteRepository::listsOnlyResumableSessions() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);

    const auto store = [&](application::SessionLifecycle lifecycle) {
        application::StoredSession session;
        session.id = domain::SessionId::generate();
        session.collectionId = *id;
        session.draft.flowId = QStringLiteral("image-wall");
        session.draft.schemaVersion = 1;
        session.lifecycle = lifecycle;
        repository_->saveSession(session, nullptr);
        return session.id;
    };

    store(application::SessionLifecycle::Active);
    store(application::SessionLifecycle::Paused);
    store(application::SessionLifecycle::Finished);
    const domain::SessionId discarded = store(application::SessionLifecycle::Discarded);

    QCOMPARE(repository_->resumableSessions(*id, &error).size(), 2);
    QVERIFY(repository_->deleteSession(discarded, &error));
    QVERIFY(!repository_->loadSession(discarded, nullptr).has_value());
}

void TestSqliteRepository::roundTripsAnOperationJournal() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    const domain::PhotoAsset asset = AssetBuilder(QStringLiteral("A"))
                                         .withJpeg()
                                         .withRaw()
                                         .withDisposition(domain::Disposition::Reject)
                                         .build();

    const domain::PlanningResult planning = domain::OperationPlanner::plan(
        *id, 1, QStringLiteral("/photos/.cullfinch-staging"), {asset});
    QVERIFY(!planning.plan.isEmpty());

    application::OperationRecord record;
    record.plan = planning.plan;
    record.state = domain::OperationState::Staging;
    for (const domain::PlannedMember& member : planning.plan.groups.first().members) {
        application::OperationMemberRecord entry;
        entry.memberId = member.memberId;
        entry.assetId = planning.plan.groups.first().assetId;
        entry.sourcePath = member.sourcePath;
        entry.lastDurableStep = QStringLiteral("planned");
        record.members.append(entry);
    }

    QVERIFY2(repository_->saveOperation(record, &error), qPrintable(error));

    const auto reloaded = repository_->loadOperation(planning.plan.id, &error);
    QVERIFY(reloaded.has_value());
    QCOMPARE(reloaded->state, domain::OperationState::Staging);
    QCOMPARE(reloaded->plan.groups.size(), 1);
    QCOMPARE(reloaded->plan.physicalFileCount(), 2);
    QCOMPARE(reloaded->members.size(), 2);
    QCOMPARE(repository_->unfinishedOperations(*id, &error).size(), 1);

    // Completed operations drop out of the recovery list.
    application::OperationRecord completed = *reloaded;
    completed.state = domain::OperationState::Completed;
    QVERIFY(repository_->saveOperation(completed, &error));
    QCOMPARE(repository_->unfinishedOperations(*id, &error).size(), 0);
}

QTEST_GUILESS_MAIN(TestSqliteRepository)
#include "tst_sqlite_repository.moc"
