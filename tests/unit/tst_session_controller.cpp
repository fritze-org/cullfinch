// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConformanceFlow.h"

#include <cullfinch/application/DispositionController.h>
#include <cullfinch/application/SessionController.h>
#include <cullfinch/flows/versus/VersusFlow.h>
#include <cullfinch/testsupport/AssetBuilder.h>
#include <cullfinch/testsupport/FakeRepository.h>

#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace cullfinch;
using cullfinch::testflows::ConformanceFlow;
using cullfinch::testflows::MisbehavingFlow;
using cullfinch::testsupport::AssetBuilder;
using cullfinch::testsupport::FakeRepository;

namespace {

domain::SelectionSnapshot snapshotFor(const domain::PhotoAssetList& assets,
                                      const domain::CollectionId& collection) {
    return domain::SelectionSnapshot::freeze(collection, 1, assets);
}

} // namespace

class TestSessionController : public QObject {
    Q_OBJECT

private slots:
    void init();

    void startsFreezesTheSelectionAndCreatesADraft();
    void refusesToStartTwice();
    void refusesAnUnknownFlow();
    void undoAndRedoRestoreExactStates();
    void aNewDecisionAfterUndoDropsTheRedoBranch();
    void rejectsAStaleAction();
    void rejectsAFlowThatTouchesUnselectedPhotos_data();
    void rejectsAFlowThatTouchesUnselectedPhotos();
    void finishMergesDraftRejectionsIntoCollectionMarks();
    void finishEarlyMarksOnlyTheDecisionsMade();
    void finishWithNoDecisionsSkipsTheMarkWrite();
    void discardLeavesEarlierMarksAlone();
    void pauseSavesTheDraftAndResumeRestoresIt();
    void resumeRefusesWhenAGroupChangedUnderneath();
    void aFailedDraftWriteKeepsTheInMemoryDraft();
    void aReadOnlyCollectionRefusesToSaveTheDraft();
    void aFailedMarkWriteLeavesMarksUnchanged();
    void aFailedSessionWriteDuringFinishLeavesMarksUnchanged();
    void aFailedUndoWriteDropsTheHistoryWithoutTouchingMarks();
    void marksCannotChangeWhileAComparisonIsActive();
    void applyRejectionsRefusesWithoutACollection();
    void persistWithNoTargetsIsANoOp();
    void theFakeRepositoryRollsBackEveryRowStoreItOffers();
    void anArbitraryNewFlowRunsThroughTheSameController();

private:
    void registerFlows();

    std::unique_ptr<FakeRepository> repository_;
    std::unique_ptr<application::FlowRegistry> registry_;
    std::unique_ptr<application::DispositionController> dispositions_;
    std::unique_ptr<application::SessionController> session_;
    domain::CollectionId collection_;
    domain::PhotoAssetList assets_;
};

void TestSessionController::registerFlows() {
    registry_->registerFlow(flows::versus::VersusFlow{}.descriptor(),
                            []() { return std::make_unique<flows::versus::VersusFlow>(); });
    registry_->registerFlow(ConformanceFlow{}.descriptor(),
                            []() { return std::make_unique<ConformanceFlow>(); });
}

void TestSessionController::init() {
    repository_ = std::make_unique<FakeRepository>();
    registry_ = std::make_unique<application::FlowRegistry>();
    dispositions_ = std::make_unique<application::DispositionController>(*repository_);
    session_ =
        std::make_unique<application::SessionController>(*registry_, *repository_, *dispositions_);
    registerFlows();

    QString error;
    const auto ensured = repository_->ensureCollection(QStringLiteral("/photos"), false, &error);
    if (!ensured.has_value()) {
        QFAIL(qPrintable(error));
    }
    collection_ = *ensured;
    assets_ = AssetBuilder::resolvedSeries(8);
    repository_->setAssets(collection_, assets_);
    dispositions_->setCollection(collection_,
                                 repository_->collectionRevision(collection_, nullptr));
}

void TestSessionController::startsFreezesTheSelectionAndCreatesADraft() {
    QSignalSpy started(session_.get(), &application::SessionController::sessionStarted);

    QString error;
    QVERIFY2(session_->start(QStringLiteral("versus-tree"), snapshotFor(assets_, collection_),
                             domain::FlowOptions{}, &error),
             qPrintable(error));

    QVERIFY(session_->isActive());
    QCOMPARE(started.size(), 1);
    QCOMPARE(session_->snapshot().size(), 8);
    QCOMPARE(session_->summary().remaining.size(), 8);
    QVERIFY(session_->sessionId().isValid());
}

void TestSessionController::refusesToStartTwice() {
    QString error;
    QVERIFY(session_->start(QStringLiteral("versus-tree"), snapshotFor(assets_, collection_),
                            domain::FlowOptions{}, &error));
    QVERIFY(!session_->start(QStringLiteral("versus-tree"), snapshotFor(assets_, collection_),
                             domain::FlowOptions{}, &error));
    QVERIFY(!error.isEmpty());
}

void TestSessionController::refusesAnUnknownFlow() {
    QString error;
    QVERIFY(!session_->start(QStringLiteral("does-not-exist"), snapshotFor(assets_, collection_),
                             domain::FlowOptions{}, &error));
    QVERIFY(!session_->isActive());
}

void TestSessionController::undoAndRedoRestoreExactStates() {
    QString error;
    session_->start(QStringLiteral("versus-tree"), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);

    const flows::versus::MatchView first =
        flows::versus::VersusFlow::pendingMatch(session_->state());
    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), first.left.toString());
    QVERIFY2(session_->dispatch(QStringLiteral("eliminate"), payload, &error), qPrintable(error));

    const domain::FlowState afterDecision = session_->state();
    QCOMPARE(session_->summary().draftRejected.size(), 1);

    session_->undoStack()->undo();
    // The exact previous match, with the same candidates, is restored.
    const flows::versus::MatchView restored =
        flows::versus::VersusFlow::pendingMatch(session_->state());
    QCOMPARE(restored.node, first.node);
    QCOMPARE(restored.left, first.left);
    QCOMPARE(restored.right, first.right);
    QCOMPARE(session_->summary().draftRejected.size(), 0);

    session_->undoStack()->redo();
    QCOMPARE(session_->state().revision, afterDecision.revision);
    QCOMPARE(session_->summary().draftRejected.size(), 1);
}

void TestSessionController::aNewDecisionAfterUndoDropsTheRedoBranch() {
    QString error;
    session_->start(QStringLiteral("versus-tree"), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);

    const flows::versus::MatchView match =
        flows::versus::VersusFlow::pendingMatch(session_->state());

    QJsonObject dropLeft;
    dropLeft.insert(QStringLiteral("assetId"), match.left.toString());
    session_->dispatch(QStringLiteral("eliminate"), dropLeft, &error);
    session_->undoStack()->undo();

    QJsonObject dropRight;
    dropRight.insert(QStringLiteral("assetId"), match.right.toString());
    QVERIFY(session_->dispatch(QStringLiteral("eliminate"), dropRight, &error));

    // The abandoned future is gone; only the new choice can be redone.
    QVERIFY(!session_->undoStack()->canRedo());
    QCOMPARE(session_->summary().draftRejected.first(), match.right);
}

void TestSessionController::rejectsAStaleAction() {
    QString error;
    session_->start(QStringLiteral("versus-tree"), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);

    const quint64 staleRevision = session_->state().revision;
    const flows::versus::MatchView match =
        flows::versus::VersusFlow::pendingMatch(session_->state());
    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), match.left.toString());
    session_->dispatch(QStringLiteral("eliminate"), payload, &error);

    // A gesture that began against the previous state is refused: one gesture
    // must never decide two matches.
    QVERIFY(!session_->dispatchAt(QStringLiteral("eliminate"), payload, staleRevision, &error));
    QCOMPARE(session_->summary().draftRejected.size(), 1);
}

void TestSessionController::rejectsAFlowThatTouchesUnselectedPhotos_data() {
    QTest::addColumn<int>("misbehaviour");
    QTest::newRow("rejects an unselected photo")
        << static_cast<int>(MisbehavingFlow::Misbehaviour::RejectAnUnselectedPhoto);
    QTest::newRow("loses photos") << static_cast<int>(MisbehavingFlow::Misbehaviour::LosePhotos);
    QTest::newRow("duplicates photos")
        << static_cast<int>(MisbehavingFlow::Misbehaviour::DuplicatePhotos);
    QTest::newRow("does not advance the revision")
        << static_cast<int>(MisbehavingFlow::Misbehaviour::NotAdvance);
}

void TestSessionController::rejectsAFlowThatTouchesUnselectedPhotos() {
    QFETCH(int, misbehaviour);
    const auto kind = static_cast<MisbehavingFlow::Misbehaviour>(misbehaviour);

    registry_->registerFlow(MisbehavingFlow{kind}.descriptor(),
                            [kind]() { return std::make_unique<MisbehavingFlow>(kind); });

    QString error;
    QVERIFY(session_->start(QStringLiteral("test-misbehaving"), snapshotFor(assets_, collection_),
                            domain::FlowOptions{}, &error));

    // The host validates the transition and refuses it; no draft change lands.
    QVERIFY2(!session_->dispatch(QStringLiteral("go"), QJsonObject{}, &error),
             "the host must refuse a transition that changes the set under comparison");
    QVERIFY(!error.isEmpty());
    QCOMPARE(session_->summary().draftRejected.size(), 0);
}

void TestSessionController::finishMergesDraftRejectionsIntoCollectionMarks() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);

    for (int decision = 0; decision < 3; ++decision) {
        QVERIFY(session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error));
    }
    const QList<domain::AssetId> expected = session_->summary().draftRejected;
    QCOMPARE(expected.size(), 3);

    QSignalSpy ended(session_.get(), &application::SessionController::sessionEnded);
    QVERIFY2(session_->finish(&error), qPrintable(error));
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended.first().at(1).toBool(), true);
    QVERIFY(!session_->isActive());

    int rejected = 0;
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        if (asset.disposition == domain::Disposition::Reject) {
            ++rejected;
            QVERIFY(expected.contains(asset.id));
        }
    }
    QCOMPARE(rejected, 3);

    // Finishing creates exactly one collection-level undo step.
    QCOMPARE(dispositions_->undoStack()->count(), 1);
    QCOMPARE(dispositions_->undoStack()->text(0),
             QStringLiteral("Apply conformance fixture rejections"));
}

void TestSessionController::finishEarlyMarksOnlyTheDecisionsMade() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);
    QVERIFY(session_->finish(&error));

    int rejected = 0;
    int neutral = 0;
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        (asset.disposition == domain::Disposition::Reject ? rejected : neutral) += 1;
    }
    // Undecided candidates stay neutral.
    QCOMPARE(rejected, 1);
    QCOMPARE(neutral, 7);
}

void TestSessionController::finishWithNoDecisionsSkipsTheMarkWrite() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    QCOMPARE(session_->summary().draftRejected.size(), 0);

    QSignalSpy ended(session_.get(), &application::SessionController::sessionEnded);
    QVERIFY2(session_->finish(&error), qPrintable(error));
    QCOMPARE(ended.size(), 1);
    QCOMPARE(ended.first().at(1).toBool(), true);

    // Nothing was rejected, so nothing about the collection marks or the
    // disposition undo history changes.
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
    QCOMPARE(dispositions_->undoStack()->count(), 0);
}

void TestSessionController::discardLeavesEarlierMarksAlone() {
    QString error;

    // An earlier comparison already marked one photo.
    QVERIFY(
        dispositions_->applyRejections({assets_.first().id}, QStringLiteral("earlier"), &error));

    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_.mid(1), collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);
    QVERIFY2(session_->discard(&error), qPrintable(error));

    int rejected = 0;
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        if (asset.disposition == domain::Disposition::Reject) {
            ++rejected;
            QCOMPARE(asset.id, assets_.first().id);
        }
    }
    QCOMPARE(rejected, 1);
}

void TestSessionController::pauseSavesTheDraftAndResumeRestoresIt() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);

    const domain::SessionId id = session_->sessionId();
    QVERIFY2(session_->pause(&error), qPrintable(error));
    QVERIFY(!session_->isActive());

    // No marks were applied by pausing.
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }

    const std::optional<application::StoredSession> stored = repository_->loadSession(id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QCOMPARE(stored->lifecycle, application::SessionLifecycle::Paused);

    QVERIFY2(session_->resume(*stored, assets_, &error), qPrintable(error));
    QCOMPARE(session_->summary().draftRejected.size(), 2);
    // Resume starts an empty undo stack at the restored draft state.
    QVERIFY(!session_->undoStack()->canUndo());
}

void TestSessionController::resumeRefusesWhenAGroupChangedUnderneath() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);
    const domain::SessionId id = session_->sessionId();
    session_->pause(&error);

    domain::PhotoAssetList changed = assets_;
    changed[3].membershipRevision = 999; // A RAW appeared or vanished.

    const std::optional<application::StoredSession> stored = repository_->loadSession(id, &error);
    if (!stored.has_value()) {
        QFAIL(qPrintable(error));
    }
    QVERIFY2(!session_->resume(*stored, changed, &error),
             "a changed file group must not silently continue a comparison");
    QVERIFY(!error.isEmpty());
    QVERIFY(!session_->isActive());
}

void TestSessionController::aFailedDraftWriteKeepsTheInMemoryDraft() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);

    repository_->failNextSessionSaves(5);
    QVERIFY2(!session_->flushPendingSave(&error), "a refused write must be reported");
    QVERIFY(!error.isEmpty());

    // The draft survives in memory and a retry is possible.
    QCOMPARE(session_->summary().draftRejected.size(), 1);
    QVERIFY(session_->hasUnsavedChanges());
}

void TestSessionController::aReadOnlyCollectionRefusesToSaveTheDraft() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);

    // The collection became read-only underneath a running comparison. No
    // write reaches storage -- not the autosave, not a pause -- and the draft
    // waits in memory until the collection is writable again.
    dispositions_->setReadOnly(true);
    QVERIFY2(!session_->flushPendingSave(&error), "a read-only collection must refuse the save");
    QVERIFY(error.contains(QStringLiteral("read-only")));
    QVERIFY(!session_->pause(&error));
    QVERIFY(session_->isActive());
    QCOMPARE(session_->summary().draftRejected.size(), 1);
    QVERIFY(session_->hasUnsavedChanges());

    dispositions_->setReadOnly(false);
    QVERIFY2(session_->flushPendingSave(&error), qPrintable(error));
    QVERIFY(!session_->hasUnsavedChanges());
}

void TestSessionController::aFailedMarkWriteLeavesMarksUnchanged() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);

    repository_->failNextDispositionWrites(1);
    QVERIFY2(!session_->finish(&error), "finishing must fail when the marks cannot be stored");

    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
    // The history is dropped rather than left describing storage incorrectly.
    QVERIFY(dispositions_->isBlocked());
    QCOMPARE(dispositions_->undoStack()->count(), 0);
}

void TestSessionController::aFailedSessionWriteDuringFinishLeavesMarksUnchanged() {
    QString error;
    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error);

    // finish() writes the Active draft once before it starts its atomic
    // section; only the write of the Finished record inside that section
    // must fail.
    repository_->failNextSessionSaves(1, 1);

    QVERIFY2(!session_->finish(&error),
             "finishing must fail when the session record cannot be saved");
    QVERIFY(!error.isEmpty());

    // The mark write that ran ahead of the failed session write must not
    // have survived on its own: a crash here must never leave marks applied
    // to a session that still looks resumable.
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        QCOMPARE(asset.disposition, domain::Disposition::Neutral);
    }
    QCOMPARE(dispositions_->undoStack()->count(), 0);
    QVERIFY(!dispositions_->isBlocked());

    // The session itself is untouched by the failed transaction and stays
    // resumable.
    QVERIFY(session_->isActive());
    QCOMPARE(session_->summary().draftRejected.size(), 1);
}

void TestSessionController::aFailedUndoWriteDropsTheHistoryWithoutTouchingMarks() {
    QString error;
    const domain::AssetId target = assets_.first().id;
    QVERIFY2(dispositions_->applyRejections({target}, QStringLiteral("test"), &error),
             qPrintable(error));
    QCOMPARE(dispositions_->undoStack()->count(), 1);

    QSignalSpy errors(dispositions_.get(), &application::DispositionController::errorOccurred);
    repository_->failNextDispositionWrites(1);

    // QUndoStack::undo() runs the command's undo() *before* the write is
    // known to have failed. The controller must survive that call -- clearing
    // the stack from inside the executing command deletes the command under
    // its own feet -- and must leave the authoritative marks alone.
    dispositions_->undoStack()->undo();

    QVERIFY(dispositions_->isBlocked());
    QCOMPARE(errors.size(), 1);
    for (const domain::PhotoAsset& asset : repository_->loadAssets(collection_, nullptr)) {
        QCOMPARE(asset.disposition,
                 asset.id == target ? domain::Disposition::Reject : domain::Disposition::Neutral);
    }

    // The history is dropped once control is back in the event loop, and no
    // further mark change is accepted until the collection is reloaded.
    QTRY_COMPARE(dispositions_->undoStack()->count(), 0);
    QVERIFY(!dispositions_->applyRejections({assets_.at(1).id}, QStringLiteral("test"), &error));
    QVERIFY(!error.isEmpty());
}

void TestSessionController::marksCannotChangeWhileAComparisonIsActive() {
    QString error;
    QVERIFY(dispositions_->isMarkingEnabled());

    session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                    domain::FlowOptions{}, &error);
    QVERIFY(!dispositions_->isMarkingEnabled());
    QVERIFY2(!dispositions_->unmark({assets_.first().id}, &error),
             "collection marks must not change underneath an active draft");

    session_->discard(&error);
    QVERIFY(dispositions_->isMarkingEnabled());
}

void TestSessionController::applyRejectionsRefusesWithoutACollection() {
    // A controller that never had setCollection() called (nothing has been
    // opened yet) must refuse a write rather than reach for an invalid id.
    application::DispositionController fresh(*repository_);
    QString error;
    QVERIFY2(!fresh.applyRejections({assets_.first().id}, QStringLiteral("test"), &error),
             "a write without an open collection must be refused");
    QVERIFY(error.contains(QStringLiteral("No collection is open")));
}

void TestSessionController::persistWithNoTargetsIsANoOp() {
    // persist() is public so the undo command type stays a plain
    // implementation detail; called with nothing to change, it must not
    // touch storage or the revision at all.
    QString error;
    QVERIFY2(dispositions_->persist({}, &error), qPrintable(error));
    QVERIFY(error.isEmpty());
}

void TestSessionController::theFakeRepositoryRollsBackEveryRowStoreItOffers() {
    // The finish() tests are only worth anything if the fake really does undo
    // what a failed transaction wrote, across every store it hands back --
    // and if a nested failure condemns the whole scope the way the real
    // repository's does, rather than riding out on an enclosing `true`.
    QString error;
    const quint64 revisionBefore = repository_->collectionRevision(collection_, nullptr);

    application::OperationRecord record;
    record.plan.id = domain::OperationId(QStringLiteral("op-rolled-back"));
    record.plan.collectionId = collection_;

    QVERIFY2(!repository_->runInTransaction(
                 [&]() {
                     QString nestedError;
                     repository_->ensureCollection(QStringLiteral("/elsewhere"), false,
                                                   &nestedError);
                     repository_->saveOperation(record, &nestedError);
                     repository_->runInTransaction([]() { return false; }, &nestedError);
                     return true;
                 },
                 &error),
             "a swallowed nested failure must still roll the outer scope back");

    QVERIFY2(!repository_->findCollection(QStringLiteral("/elsewhere"), &error).has_value(),
             "a collection created inside the failed scope must not survive it");
    QVERIFY2(!repository_->loadOperation(record.plan.id, &error).has_value(),
             "an operation saved inside the failed scope must not survive it");
    QCOMPARE(repository_->collectionRevision(collection_, nullptr), revisionBefore);

    // The journal is deliberately outside the rollback: it records what was
    // attempted, which is what a crash-recovery test needs to inspect.
    QCOMPARE(repository_->operationJournal().size(), 1);
}

void TestSessionController::anArbitraryNewFlowRunsThroughTheSameController() {
    // The conformance fixture is never mentioned by the controller: it is
    // driven entirely through the registered engine and the shared envelope.
    QString error;
    QVERIFY(session_->start(QLatin1String(ConformanceFlow::kId), snapshotFor(assets_, collection_),
                            domain::FlowOptions{}, &error));

    while (!session_->summary().complete) {
        QVERIFY(session_->dispatch(QStringLiteral("drop"), QJsonObject{}, &error));
    }
    QCOMPARE(session_->summary().draftRejected.size(), 8);
    QVERIFY(session_->finish(&error));
}

QTEST_GUILESS_MAIN(TestSessionController)
#include "tst_session_controller.moc"
