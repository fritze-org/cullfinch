// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/SqliteRepository.h>
#include <cullfinch/testsupport/AssetBuilder.h>

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

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
    void aSymlinkToTheRootIsTheSameCollection();
    void aRootStoredBeforeItExistedIsStillFound();
    void storesAndReloadsAssetsWithTheirMembers();
    void keepsMarksWhenMembershipIsUnchanged();
    void invalidatesMarksWhenMembershipChanges();
    void marksAnAssetStaleWhenAMemberDisappears();
    void refusesAMarkChangeAgainstAStaleRevision();
    void runInTransactionRollsBackMarksWhenTheEnclosingActionFails();
    void aFailedMarkUpdateRollsBackTheWholeTransaction();
    void aFailedRevisionAdvanceRollsBackTheWholeTransaction();
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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    QVERIFY(id->isValid());
    QVERIFY(repository_->collectionRevision(*id, &error) > 0);
}

void TestSqliteRepository::reopeningIsIdempotent() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos/a"), false, &error);
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    repository_->close();

    infrastructure::SqliteRepository reopened(databaseFile_);
    QVERIFY2(reopened.open(&error), qPrintable(error));
    const auto again = reopened.ensureCollection(QStringLiteral("/photos/a"), false, &error);
    if (!again.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(again->toString(), id->toString());
}

void TestSqliteRepository::reusesTheCollectionIdentityForTheSameRoot() {
    QString error;
    const auto first = repository_->ensureCollection(QStringLiteral("/photos/a"), false, &error);
    const auto second = repository_->ensureCollection(QStringLiteral("/photos/a"), true, &error);
    const auto other = repository_->ensureCollection(QStringLiteral("/photos/b"), false, &error);
    if (!first.has_value() || !second.has_value() || !other.has_value()) {
        QFAIL(qPrintable(error));
    }

    QCOMPARE(first->toString(), second->toString());
    QVERIFY(other->toString() != first->toString());
}

void TestSqliteRepository::aSymlinkToTheRootIsTheSameCollection() {
    const QString real = directory_.filePath(QStringLiteral("photos"));
    const QString link = directory_.filePath(QStringLiteral("photos-link"));
    QVERIFY(QDir().mkpath(real));
    QVERIFY(QFile::link(real, link));

    // The writer opened the real path. A read-only instance that opened the
    // symlink is looking at the same directory and must find the same row;
    // the lock already treats the two spellings as one collection.
    QString error;
    const auto stored = repository_->ensureCollection(real, false, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    const auto viaLink = repository_->findCollection(link, &error);
    if (!viaLink.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(viaLink->toString(), stored->toString());

    // And once the writer is gone, opening through the symlink for writing
    // reconciles onto that row instead of creating a second one.
    const auto ensured = repository_->ensureCollection(link, true, &error);
    if (!ensured.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(ensured->toString(), stored->toString());
}

void TestSqliteRepository::aRootStoredBeforeItExistedIsStillFound() {
    // A row written under the absolute spelling -- which is what a root that
    // could not be resolved at the time got, and what every row written
    // before the identity was canonical has -- is still found once the
    // spelling resolves elsewhere.
    const QString real = directory_.filePath(QStringLiteral("photos-later"));
    const QString link = directory_.filePath(QStringLiteral("photos-later-link"));
    QString error;
    const auto stored = repository_->ensureCollection(link, false, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }

    QVERIFY(QDir().mkpath(real));
    QVERIFY(QFile::link(real, link));
    const auto found = repository_->findCollection(link, &error);
    if (!found.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(found->toString(), stored->toString());
    const auto ensured = repository_->ensureCollection(link, false, &error);
    if (!ensured.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(ensured->toString(), stored->toString());
}

void TestSqliteRepository::storesAndReloadsAssetsWithTheirMembers() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(3);

    quint64 revision = 0;
    QVERIFY2(repository_->reconcileAssets(*id, assets, nullptr, &revision, &error),
             qPrintable(error));
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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);

    quint64 revision = 0;
    QVERIFY2(repository_->reconcileAssets(*id, assets, nullptr, &revision, &error),
             qPrintable(error));
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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);

    quint64 revision = 0;
    QVERIFY2(repository_->reconcileAssets(*id, assets, nullptr, &revision, &error),
             qPrintable(error));

    quint64 ignored = 0;
    QVERIFY2(!repository_->applyDispositions(*id, revision - 1, {assets.first().id}, {}, &ignored,
                                             &error),
             "a mark change must be guarded by the collection revision");
    QVERIFY(!error.isEmpty());

    for (const domain::PhotoAsset& asset : repository_->loadAssets(*id, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
}

void TestSqliteRepository::runInTransactionRollsBackMarksWhenTheEnclosingActionFails() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);

    quint64 revision = 0;
    QVERIFY2(repository_->reconcileAssets(*id, assets, nullptr, &revision, &error),
             qPrintable(error));

    // applyDispositions must nest inside an enclosing runInTransaction scope
    // rather than starting a second, conflicting one -- this is what lets
    // SessionController::finish() apply marks and save the session record as
    // one atomic unit.
    quint64 writtenRevision = revision;
    QVERIFY2(!repository_->runInTransaction(
                 [&]() {
                     if (!repository_->applyDispositions(*id, revision, {assets.first().id}, {},
                                                         &writtenRevision, &error)) {
                         return false;
                     }
                     // Something else sharing this transaction fails; the mark
                     // write above must not survive on its own.
                     return false;
                 },
                 &error),
             "the enclosing action's failure must roll back every write in the scope");

    for (const domain::PhotoAsset& asset : repository_->loadAssets(*id, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
    QCOMPARE(repository_->collectionRevision(*id, nullptr), revision);
}

void TestSqliteRepository::aFailedMarkUpdateRollsBackTheWholeTransaction() {
    // A repository of its own, opened under a connection name this test can
    // also reach: corrupting the schema through that same connection is a
    // real SQL failure at the point applyDispositions writes the marks,
    // rather than something injected around the repository's back.
    const QString connectionName = QStringLiteral("sqlite-repo-corrupt-marks");
    infrastructure::SqliteRepository repo(
        directory_.filePath(QStringLiteral("corrupt-marks.sqlite")), connectionName);
    QString error;
    QVERIFY2(repo.open(&error), qPrintable(error));

    const auto id = repo.ensureCollection(QStringLiteral("/photos"), false, &error);
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);
    quint64 revision = 0;
    QVERIFY2(repo.reconcileAssets(*id, assets, nullptr, &revision, &error), qPrintable(error));

    {
        QSqlQuery drop(QSqlDatabase::database(connectionName));
        QVERIFY2(drop.exec(QStringLiteral("DROP TABLE assets")),
                 qPrintable(drop.lastError().text()));
    }

    quint64 ignored = 0;
    QVERIFY2(!repo.applyDispositions(*id, revision, {assets.first().id}, {}, &ignored, &error),
             "a real SQL failure while writing marks must be reported, not ignored");
    QVERIFY(error.contains(QStringLiteral("Storing deletion marks failed")));
    // The whole transaction rolled back with the failed write: the
    // collection's revision never advanced.
    QCOMPARE(repo.collectionRevision(*id, nullptr), revision);
}

void TestSqliteRepository::aFailedRevisionAdvanceRollsBackTheWholeTransaction() {
    const QString connectionName = QStringLiteral("sqlite-repo-corrupt-revision");
    infrastructure::SqliteRepository repo(
        directory_.filePath(QStringLiteral("corrupt-revision.sqlite")), connectionName);
    QString error;
    QVERIFY2(repo.open(&error), qPrintable(error));

    const auto id = repo.ensureCollection(QStringLiteral("/photos"), false, &error);
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
    const domain::PhotoAssetList assets = AssetBuilder::resolvedSeries(2);
    quint64 revision = 0;
    QVERIFY2(repo.reconcileAssets(*id, assets, nullptr, &revision, &error), qPrintable(error));

    // The disposition UPDATE succeeds; only the later revision bump fails --
    // and the whole transaction, marks included, must still roll back.
    {
        QSqlQuery trigger(QSqlDatabase::database(connectionName));
        QVERIFY2(trigger.exec(QStringLiteral(
                     "CREATE TRIGGER fail_revision BEFORE UPDATE OF revision ON collections "
                     "BEGIN SELECT RAISE(ABORT, 'boom'); END;")),
                 qPrintable(trigger.lastError().text()));
    }

    quint64 ignored = 0;
    QVERIFY2(!repo.applyDispositions(*id, revision, {assets.first().id}, {}, &ignored, &error),
             "a failure advancing the revision must roll back the mark write too");
    QVERIFY(error.contains(QStringLiteral("Advancing the collection revision failed")));

    for (const domain::PhotoAsset& asset : repo.loadAssets(*id, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
    QCOMPARE(repo.collectionRevision(*id, nullptr), revision);
}

void TestSqliteRepository::roundTripsASessionDraft() {
    QString error;
    const auto id = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
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
    if (!reloaded.has_value()) {
        QFAIL(qPrintable(error));
    }
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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }

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
    if (!id.has_value()) {
        QFAIL(qPrintable(error));
    }
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
    if (!reloaded.has_value()) {
        QFAIL(qPrintable(error));
    }
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
