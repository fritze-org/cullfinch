// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/OperationController.h>
#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/testsupport/FakeRepository.h>
#include <cullfinch/testsupport/FakeTrashAdapter.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

#include <optional>

using namespace cullfinch;
using cullfinch::testsupport::FakeRepository;
using cullfinch::testsupport::FakeTrashAdapter;
using cullfinch::testsupport::TempCollection;

/// The controller with the real staging executor underneath: what the journal
/// in the repository looks like as a reviewed plan runs, stops, and recovers.
class TestOperationController : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void executesAReviewedPlanAndJournalsEveryStep();
    void aRefusedJournalWriteStopsTheRunBeforeAnythingMoves();
    void aChangedGroupInvalidatesThePlan();
    void aMarkClearedAfterReviewInvalidatesThePlan();
    void anIdleRescanDoesNotInvalidateThePlan();
    void recoverPutsAnInterruptedOperationBack();
    void retryTrashMovesAGroupTrashRefusedTheFirstTime();
    void retryTrashRefusesAPlanWithAGroupLeftAtItsOriginalPaths();
    void confirmTrashedSettlesAnOutcomeNothingOnDiskCanShow();
    void aRunThatLeftNothingBehindIsNotARecoveryTask();

private:
    [[nodiscard]] domain::PhotoAssetList scan() const;
    [[nodiscard]] domain::OperationPlan reviewedPlan();
    /// A plan for one photo, which is the only shape in which every group can
    /// be staged at once: execution stops at the first group that leaves
    /// recoverable work, so a two-group plan never gets that far.
    [[nodiscard]] domain::OperationPlan reviewedPlanFor(const QString& stem);
    /// The journal as it stood just before Trash was asked -- what a crash in
    /// the uncertain interval leaves behind.
    [[nodiscard]] std::optional<application::OperationRecord>
    snapshotBeforeTrash(const QList<application::OperationRecord>& journal) const;

    std::unique_ptr<TempCollection> collection_;
    std::unique_ptr<FakeRepository> repository_;
    std::unique_ptr<FakeTrashAdapter> trash_;
    std::unique_ptr<infrastructure::StagingExecutor> executor_;
    std::unique_ptr<application::OperationController> controller_;
    domain::CollectionId collectionId_;
};

void TestOperationController::init() {
    collection_ = std::make_unique<TempCollection>();
    collection_->addJpeg(QStringLiteral("A.JPG"));
    collection_->addRaw(QStringLiteral("A.RAF"));
    collection_->addJpeg(QStringLiteral("B.JPG"));

    repository_ = std::make_unique<FakeRepository>();
    QString error;
    const auto ensured = repository_->ensureCollection(collection_->path(), false, &error);
    if (!ensured.has_value()) {
        QFAIL(qPrintable(error));
    }
    collectionId_ = *ensured;
    trash_ = std::make_unique<FakeTrashAdapter>();
    executor_ = std::make_unique<infrastructure::StagingExecutor>(*trash_);
    controller_ = std::make_unique<application::OperationController>(*repository_, *executor_);
}

void TestOperationController::cleanup() {
    controller_.reset();
    executor_.reset();
    trash_.reset();
    repository_.reset();
    collection_.reset();
}

domain::PhotoAssetList TestOperationController::scan() const {
    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection_->path(), false, &error);
    const domain::StemAssociationResolver resolver;
    domain::PhotoAssetList assets =
        resolver.resolve(collectionId_, files, domain::AssociationConfig::defaults(), true).assets;
    for (domain::PhotoAsset& asset : assets) {
        asset.disposition = domain::Disposition::Reject;
    }
    return assets;
}

domain::OperationPlan TestOperationController::reviewedPlan() {
    const domain::PlanningResult planning = controller_->review(
        collectionId_, 1, QDir(collection_->path()).absoluteFilePath(QStringLiteral(".staging")),
        scan());
    return planning.plan;
}

domain::OperationPlan TestOperationController::reviewedPlanFor(const QString& stem) {
    domain::PhotoAssetList only;
    for (const domain::PhotoAsset& asset : scan()) {
        if (asset.stem == stem) {
            only.append(asset);
        }
    }
    const domain::PlanningResult planning = controller_->review(
        collectionId_, 1, QDir(collection_->path()).absoluteFilePath(QStringLiteral(".staging")),
        only);
    return planning.plan;
}

std::optional<application::OperationRecord> TestOperationController::snapshotBeforeTrash(
    const QList<application::OperationRecord>& journal) const {
    for (const application::OperationRecord& record : journal) {
        // Trashing with no Trash path recorded yet: the group is complete in
        // staging and the platform has not been asked about it.
        if (record.state == domain::OperationState::Trashing && record.trashPath.isEmpty()) {
            return record;
        }
    }
    return std::nullopt;
}

void TestOperationController::executesAReviewedPlanAndJournalsEveryStep() {
    const domain::OperationPlan plan = reviewedPlan();
    QCOMPARE(plan.logicalPhotoCount(), 2);
    QSignalSpy progress(controller_.get(), &application::OperationController::progressChanged);

    QString error;
    QVERIFY2(controller_->execute(plan, scan(), &error), qPrintable(error));
    QCOMPARE(trash_->callCount(), 2);
    QCOMPARE(progress.size(), 3); // 0 of 2, 1 of 2, 2 of 2.

    // The plan, then per group: the intent, one confirmation per file,
    // Staged, Trashing, the Trash outcome, and the controller's own write of
    // the returned record; then Completed.
    const QList<application::OperationRecord>& journal = repository_->operationJournal();
    QCOMPARE(journal.size(), 1 + (1 + 2 + 1 + 1 + 1 + 1) + (1 + 1 + 1 + 1 + 1 + 1) + 1);
    QCOMPARE(journal.first().state, domain::OperationState::Planned);
    QCOMPARE(journal.last().state, domain::OperationState::Completed);

    // No journal entry ever claims a file is staged while it is still at
    // its source: the journal never runs ahead of the filesystem.
    int stagedSeen = 0;
    for (const application::OperationRecord& snapshot : journal) {
        for (const application::OperationMemberRecord& member : snapshot.members) {
            if (member.lastDurableStep == QLatin1String("staged")) {
                ++stagedSeen;
                QVERIFY(!member.stagingPath.isEmpty());
            }
        }
    }
    QVERIFY(stagedSeen > 0);

    const auto stored = repository_->loadOperation(plan.id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->state, domain::OperationState::Completed);
    QVERIFY(repository_->unfinishedOperations(collectionId_, &error).isEmpty());
}

void TestOperationController::aRefusedJournalWriteStopsTheRunBeforeAnythingMoves() {
    const domain::OperationPlan plan = reviewedPlan();

    // The plan record itself is written; the first intent write is refused.
    repository_->failOperationSaves(1, 1);
    QSignalSpy errors(controller_.get(), &application::OperationController::errorOccurred);
    QString error;
    QVERIFY(!controller_->execute(plan, scan(), &error));
    QVERIFY2(error.contains(QStringLiteral("journal")), qPrintable(error));

    // Nothing moved and nothing reached Trash.
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.RAF"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("B.JPG"))));
    QCOMPARE(trash_->callCount(), 0);

    // The refusal itself is journalled once the repository accepts writes
    // again, so the record does not stay "planned" forever.
    const auto stored = repository_->loadOperation(plan.id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->state, domain::OperationState::Failed);
}

void TestOperationController::aChangedGroupInvalidatesThePlan() {
    const domain::OperationPlan plan = reviewedPlan();
    QSignalSpy invalidated(controller_.get(), &application::OperationController::planInvalidated);

    // A RAW disappears between review and execution.
    QVERIFY(QFile::remove(collection_->filePath(QStringLiteral("A.RAF"))));
    QString error;
    QVERIFY(!controller_->execute(plan, scan(), &error));
    QCOMPARE(invalidated.size(), 1);
    QCOMPARE(trash_->callCount(), 0);
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("B.JPG"))));
    // Nothing was recorded for a plan that never started.
    QVERIFY(!repository_->loadOperation(plan.id, &error).has_value());
}

void TestOperationController::aMarkClearedAfterReviewInvalidatesThePlan() {
    const domain::OperationPlan plan = reviewedPlan();
    QSignalSpy invalidated(controller_.get(), &application::OperationController::planInvalidated);

    // The reject mark on B is cleared from another window between review and
    // execution; nothing on disk changes.
    domain::PhotoAssetList current = scan();
    for (domain::PhotoAsset& asset : current) {
        if (asset.stem == QStringLiteral("B")) {
            asset.disposition = domain::Disposition::Neutral;
        }
    }

    QString error;
    QVERIFY(!controller_->execute(plan, current, &error));
    QCOMPARE(invalidated.size(), 1);
    QCOMPARE(trash_->callCount(), 0);
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.RAF"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("B.JPG"))));
    // Nothing was recorded for a plan that never started.
    QVERIFY(!repository_->loadOperation(plan.id, &error).has_value());
}

void TestOperationController::anIdleRescanDoesNotInvalidateThePlan() {
    // Seed the repository with the marks review saw, so reconcileAssets has
    // something to carry the disposition forward from.
    repository_->setAssets(collectionId_, scan());
    const domain::OperationPlan plan = reviewedPlan();

    // A window activation triggers a rescan that changes nothing structurally
    // or in the marks, but still bumps the collection revision.
    domain::PhotoAssetList merged;
    quint64 newRevision = 0;
    QString error;
    QVERIFY2(repository_->reconcileAssets(collectionId_, scan(), &merged, &newRevision, &error),
             qPrintable(error));
    QVERIFY(newRevision > plan.collectionRevision);

    QVERIFY2(controller_->execute(plan, merged, &error), qPrintable(error));
    QCOMPARE(trash_->callCount(), 2);
}

void TestOperationController::recoverPutsAnInterruptedOperationBack() {
    const domain::OperationPlan plan = reviewedPlan();
    trash_->failNextCalls(1);

    QString error;
    QVERIFY(!controller_->execute(plan, scan(), &error));
    QCOMPARE(repository_->unfinishedOperations(collectionId_, &error).size(), 1);
    QVERIFY(!QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));

    QVERIFY2(controller_->recover(plan.id, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.RAF"))));
    const auto stored = repository_->loadOperation(plan.id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->state, domain::OperationState::Planned);

    // An operation that still needs a person is reported as such.
    QVERIFY(!controller_->recover(domain::OperationId(QStringLiteral("nope")), &error));
    QVERIFY(!error.isEmpty());
}

void TestOperationController::retryTrashMovesAGroupTrashRefusedTheFirstTime() {
    const domain::OperationPlan plan = reviewedPlanFor(QStringLiteral("A"));
    QCOMPARE(plan.logicalPhotoCount(), 1);
    trash_->failNextCalls(1);

    QString error;
    QVERIFY(!controller_->execute(plan, scan(), &error));
    // The complete group is retained in staging: neither file came back, and
    // neither was deleted.
    QVERIFY(!QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));
    QVERIFY(!QFileInfo::exists(collection_->filePath(QStringLiteral("A.RAF"))));

    const QList<application::OperationRecord> pending = controller_->needingRecovery(collectionId_);
    QCOMPARE(pending.size(), 1);
    const application::RecoveryOffers offers = application::recoveryOffersFor(pending.first());
    QVERIFY(offers.restore);
    QVERIFY(offers.retryTrash);
    QVERIFY(!offers.confirmTrashed);

    QVERIFY2(controller_->retryTrash(plan.id, &error), qPrintable(error));
    QCOMPARE(trash_->callCount(), 2);
    QVERIFY(!QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));

    const auto stored = repository_->loadOperation(plan.id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->state, domain::OperationState::Completed);
    QVERIFY(controller_->needingRecovery(collectionId_).isEmpty());
}

void TestOperationController::retryTrashRefusesAPlanWithAGroupLeftAtItsOriginalPaths() {
    // Two groups, and Trash refuses the first: the run stops, so the second
    // group never leaves its original paths. The plan no longer describes the
    // work on disk, and asking Trash for half of it would make that permanent.
    const domain::OperationPlan plan = reviewedPlan();
    QCOMPARE(plan.logicalPhotoCount(), 2);
    trash_->failNextCalls(1);

    QString error;
    QVERIFY(!controller_->execute(plan, scan(), &error));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("B.JPG"))));

    const QList<application::OperationRecord> pending = controller_->needingRecovery(collectionId_);
    QCOMPARE(pending.size(), 1);
    const application::RecoveryOffers offers = application::recoveryOffersFor(pending.first());
    QVERIFY(offers.restore);
    QVERIFY2(!offers.retryTrash, "retrying Trash was offered for a plan that is not all staged");

    // Refused even when asked for directly, and nothing more is trashed.
    QVERIFY(!controller_->retryTrash(plan.id, &error));
    QVERIFY2(error.contains(QStringLiteral("not in staging")), qPrintable(error));
    QCOMPARE(trash_->callCount(), 1);
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("B.JPG"))));

    // Restore is the way out, and it puts the staged group back.
    QVERIFY2(controller_->recover(plan.id, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.JPG"))));
    QVERIFY(QFileInfo::exists(collection_->filePath(QStringLiteral("A.RAF"))));
}

void TestOperationController::confirmTrashedSettlesAnOutcomeNothingOnDiskCanShow() {
    // The platform is allowed to report no Trash path, and a crash between the
    // Trash call and its journal write then leaves a record that cannot say
    // where the group went.
    trash_->setReportsPath(false);
    const domain::OperationPlan plan = reviewedPlanFor(QStringLiteral("A"));

    QList<application::OperationRecord> journal;
    QObject::connect(
        controller_.get(), &application::OperationController::recordChanged, controller_.get(),
        [&journal](const application::OperationRecord& record) { journal.append(record); });

    QString error;
    QVERIFY2(controller_->execute(plan, scan(), &error), qPrintable(error));
    const std::optional<application::OperationRecord> interrupted = snapshotBeforeTrash(journal);
    if (!interrupted.has_value()) {
        QFAIL("the journal never recorded the moment just before Trash was asked");
    }

    // Rewind the journal to that moment and reconcile, as a restart would.
    QVERIFY2(repository_->saveOperation(*interrupted, &error), qPrintable(error));
    QVERIFY(!controller_->recover(plan.id, &error));
    QVERIFY2(error.contains(QStringLiteral("Trash")), qPrintable(error));
    QCOMPARE(trash_->callCount(), 1); // Nothing was deleted again on a guess.

    const QList<application::OperationRecord> pending = controller_->needingRecovery(collectionId_);
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().state, domain::OperationState::NeedsRecovery);
    const application::RecoveryOffers offers = application::recoveryOffersFor(pending.first());
    QVERIFY(offers.confirmTrashed);
    QVERIFY2(!offers.restore, "restoring was offered for files that are not in staging");
    QVERIFY2(!offers.retryTrash, "retrying Trash was offered for a group nobody can find");

    // A person has looked in the Trash. That answer, and nothing else, settles
    // it -- and it still moves nothing.
    QVERIFY2(controller_->confirmTrashed(plan.id, &error), qPrintable(error));
    QCOMPARE(trash_->callCount(), 1);

    const auto stored = repository_->loadOperation(plan.id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->state, domain::OperationState::Completed);
    QVERIFY(controller_->needingRecovery(collectionId_).isEmpty());
}

void TestOperationController::aRunThatLeftNothingBehindIsNotARecoveryTask() {
    const domain::OperationPlan plan = reviewedPlan();
    repository_->failOperationSaves(1, 1);

    QString error;
    QVERIFY(!controller_->execute(plan, scan(), &error));

    // Failed: the journal write was refused before anything moved. It is an
    // unfinished operation, but there is nothing on disk to repair, so it is
    // not put in front of a person as one.
    const auto stored = repository_->loadOperation(plan.id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->state, domain::OperationState::Failed);
    QCOMPARE(repository_->unfinishedOperations(collectionId_, &error).size(), 1);
    QVERIFY(controller_->needingRecovery(collectionId_).isEmpty());
    QVERIFY(!application::recoveryOffersFor(*stored).any());
}

QTEST_MAIN(TestOperationController)
#include "tst_operation_controller.moc"
