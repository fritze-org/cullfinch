// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/versus/VersusFlow.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTest>

using namespace cullfinch;
using cullfinch::flows::versus::MatchView;
using cullfinch::flows::versus::VersusFlow;

namespace {

domain::SelectionSnapshot snapshotOf(int count) {
    domain::SelectionSnapshot snapshot;
    snapshot.collectionId = domain::CollectionId(QStringLiteral("c1"));
    snapshot.collectionRevision = 1;
    for (int index = 0; index < count; ++index) {
        const domain::AssetId id(QStringLiteral("asset-%1").arg(index, 3, 10, QLatin1Char('0')));
        snapshot.orderedAssetIds.append(id);
        snapshot.membershipRevisions.insert(id, 1);
    }
    return snapshot;
}

domain::FlowAction eliminate(const domain::AssetId& id, quint64 revision) {
    domain::FlowAction action;
    action.flowId = QLatin1String(flows::versus::kFlowId);
    action.name = QLatin1String(flows::versus::kActionEliminate);
    action.expectedStateRevision = revision;
    action.payload.insert(QStringLiteral("assetId"), id.toString());
    return action;
}

/// Play a whole bracket, always eliminating the left candidate.
domain::FlowState playToCompletion(const VersusFlow& flow, domain::FlowState state,
                                   int* decisions) {
    *decisions = 0;
    while (true) {
        const MatchView match = VersusFlow::pendingMatch(state);
        if (!match.isValid()) {
            return state;
        }
        const domain::TransitionResult result =
            flow.reduce(state, eliminate(match.left, state.revision));
        if (!result.accepted) {
            return state;
        }
        state = result.state;
        ++*decisions;
    }
}

} // namespace

class TestVersusFlow : public QObject {
    Q_OBJECT

private slots:
    void cannotStartWithNoCandidates();
    void singleCandidateIsImmediatelyTheSurvivor();
    void completionTakesExactlyOneDecisionPerEliminatedPhoto_data();
    void completionTakesExactlyOneDecisionPerEliminatedPhoto();
    void byesAdvanceWithoutAFakeOpponent();
    void doesNotReplayTheWinnerAgainstEveryNewcomer();
    void remainingAndRejectedPartitionTheInputAtEveryStep();
    void refusesACandidateFromAnotherMatch();
    void refusesAnActionAfterTheBracketIsComplete();
    void bracketIsPersistedNotRegenerated();
    void restoreRejectsAnUnknownStateVersion();
    void restoreRejectsABracketSizeBeyondWhatRoundIndexingCanShift();
    void finishingEarlyMarksOnlyDecidedLosers();
};

void TestVersusFlow::cannotStartWithNoCandidates() {
    const VersusFlow flow;
    const domain::ValidationResult result = flow.validate(snapshotOf(0), domain::FlowOptions{});
    QVERIFY(!result.valid);
    QVERIFY(!result.message.isEmpty());
}

void TestVersusFlow::singleCandidateIsImmediatelyTheSurvivor() {
    const VersusFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(1);
    QVERIFY(flow.validate(selection, domain::FlowOptions{}).valid);

    const domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});
    const domain::FlowSummary summary = flow.summarise(state);

    QVERIFY(!VersusFlow::pendingMatch(state).isValid());
    QVERIFY(summary.complete);
    QCOMPARE(summary.decisionsMade, 0);
    QCOMPARE(summary.draftRejected.size(), 0);
    QCOMPARE(summary.remaining.size(), 1);
    QCOMPARE(VersusFlow::survivor(state), selection.orderedAssetIds.first());
}

void TestVersusFlow::completionTakesExactlyOneDecisionPerEliminatedPhoto_data() {
    QTest::addColumn<int>("count");
    // Powers of two and everything awkward in between.
    for (int count : {2, 3, 4, 5, 6, 7, 8, 9, 11, 13, 16, 17, 31, 32, 33, 64}) {
        QTest::newRow(qPrintable(QStringLiteral("N=%1").arg(count))) << count;
    }
}

void TestVersusFlow::completionTakesExactlyOneDecisionPerEliminatedPhoto() {
    QFETCH(int, count);

    const VersusFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(count);
    int decisions = 0;
    const domain::FlowState finished =
        playToCompletion(flow, flow.initialise(selection, domain::FlowOptions{}), &decisions);

    const domain::FlowSummary summary = flow.summarise(finished);
    QVERIFY(summary.complete);
    QCOMPARE(decisions, count - 1);
    QCOMPARE(summary.decisionsMade, count - 1);
    QCOMPARE(summary.remaining.size(), 1);
    QCOMPARE(summary.draftRejected.size(), count - 1);

    // Exactly one survivor, and the rejected identifiers are all distinct.
    const QSet<domain::AssetId> rejected(summary.draftRejected.cbegin(),
                                         summary.draftRejected.cend());
    QCOMPARE(rejected.size(), summary.draftRejected.size());
    QVERIFY(VersusFlow::survivor(finished).isValid());
    QVERIFY(!rejected.contains(VersusFlow::survivor(finished)));
}

void TestVersusFlow::byesAdvanceWithoutAFakeOpponent() {
    const VersusFlow flow;
    // Five candidates need an eight-slot bracket, so three positions are byes.
    const domain::FlowState state = flow.initialise(snapshotOf(5), domain::FlowOptions{});

    const QList<domain::AssetId> positions = VersusFlow::bracketSlots(state);
    QCOMPARE(positions.size(), 8);

    int real = 0;
    for (const domain::AssetId& slot : positions) {
        if (slot.isValid()) {
            ++real;
        }
    }
    QCOMPARE(real, 5);

    // Every displayed match has two real candidates: a bye never appears.
    domain::FlowState current = state;
    while (true) {
        const MatchView match = VersusFlow::pendingMatch(current);
        if (!match.isValid()) {
            break;
        }
        QVERIFY(match.left.isValid());
        QVERIFY(match.right.isValid());
        QVERIFY(!(match.left == match.right));
        current = flow.reduce(current, eliminate(match.left, current.revision)).state;
    }
    QCOMPARE(flow.summarise(current).draftRejected.size(), 4);
}

void TestVersusFlow::doesNotReplayTheWinnerAgainstEveryNewcomer() {
    const VersusFlow flow;
    domain::FlowState state = flow.initialise(snapshotOf(8), domain::FlowOptions{});

    // In a balanced bracket of eight, the first four decisions are all in the
    // first round: no candidate appears in two consecutive matches.
    QSet<domain::AssetId> firstRoundParticipants;
    for (int decision = 0; decision < 4; ++decision) {
        const MatchView match = VersusFlow::pendingMatch(state);
        QCOMPARE(match.round, 1);
        QVERIFY(!firstRoundParticipants.contains(match.left));
        QVERIFY(!firstRoundParticipants.contains(match.right));
        firstRoundParticipants.insert(match.left);
        firstRoundParticipants.insert(match.right);
        state = flow.reduce(state, eliminate(match.left, state.revision)).state;
    }
    QCOMPARE(firstRoundParticipants.size(), 8);
    QCOMPARE(VersusFlow::pendingMatch(state).round, 2);
}

void TestVersusFlow::remainingAndRejectedPartitionTheInputAtEveryStep() {
    const VersusFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(11);
    const QSet<domain::AssetId> input(selection.orderedAssetIds.cbegin(),
                                      selection.orderedAssetIds.cend());

    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});
    while (true) {
        const domain::FlowSummary summary = flow.summarise(state);
        const QSet<domain::AssetId> remaining(summary.remaining.cbegin(), summary.remaining.cend());
        const QSet<domain::AssetId> rejected(summary.draftRejected.cbegin(),
                                             summary.draftRejected.cend());

        QVERIFY2(!remaining.intersects(rejected), "remaining and rejected must be disjoint");
        QCOMPARE(remaining | rejected, input);

        const MatchView match = VersusFlow::pendingMatch(state);
        if (!match.isValid()) {
            break;
        }
        state = flow.reduce(state, eliminate(match.right, state.revision)).state;
    }
}

void TestVersusFlow::refusesACandidateFromAnotherMatch() {
    const VersusFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(8);
    const domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});
    const MatchView match = VersusFlow::pendingMatch(state);

    domain::AssetId stranger;
    for (const domain::AssetId& id : selection.orderedAssetIds) {
        if (!(id == match.left) && !(id == match.right)) {
            stranger = id;
            break;
        }
    }
    QVERIFY(stranger.isValid());

    const domain::TransitionResult result = flow.reduce(state, eliminate(stranger, state.revision));
    QVERIFY(!result.accepted);
    QVERIFY(!result.message.isEmpty());
}

void TestVersusFlow::refusesAnActionAfterTheBracketIsComplete() {
    const VersusFlow flow;
    int decisions = 0;
    const domain::FlowState finished =
        playToCompletion(flow, flow.initialise(snapshotOf(4), domain::FlowOptions{}), &decisions);

    // A repeated key or a late click against a decided match decides nothing.
    const domain::TransitionResult result =
        flow.reduce(finished, eliminate(VersusFlow::survivor(finished), finished.revision));
    QVERIFY(!result.accepted);
}

void TestVersusFlow::bracketIsPersistedNotRegenerated() {
    const VersusFlow flow;
    const domain::FlowState state = flow.initialise(snapshotOf(6), domain::FlowOptions{});

    domain::VersionedFlowState saved;
    saved.flowId = state.flowId;
    saved.schemaVersion = state.schemaVersion;
    saved.revision = state.revision;
    saved.payload = state.payload;

    const domain::RestoreResult restored = flow.restore(saved);
    QVERIFY(restored.restored);
    // The restored bracket is the same bracket, position for position.
    QCOMPARE(VersusFlow::bracketSlots(restored.state), VersusFlow::bracketSlots(state));
    QCOMPARE(VersusFlow::pendingMatch(restored.state).node, VersusFlow::pendingMatch(state).node);
}

void TestVersusFlow::restoreRejectsAnUnknownStateVersion() {
    const VersusFlow flow;
    domain::VersionedFlowState saved;
    saved.flowId = QLatin1String(flows::versus::kFlowId);
    saved.schemaVersion = flows::versus::kStateSchemaVersion + 1;

    const domain::RestoreResult result = flow.restore(saved);
    QVERIFY(!result.restored);
    QVERIFY(result.message.contains(QStringLiteral("schema version")));

    // State belonging to another flow is never interpreted as this one.
    saved.flowId = QStringLiteral("image-wall");
    saved.schemaVersion = flows::versus::kStateSchemaVersion;
    QVERIFY(!flow.restore(saved).restored);
}

void TestVersusFlow::restoreRejectsABracketSizeBeyondWhatRoundIndexingCanShift() {
    // A bracketSize this large would need over a billion "slots" entries to
    // pass the length check that follows it, so the bound is checked first:
    // this stays cheap to test and never depends on constructing that array.
    QJsonObject payload;
    payload.insert(QStringLiteral("bracketSize"), (1 << 30) + 1);
    payload.insert(QStringLiteral("slots"), QJsonArray{});

    domain::VersionedFlowState saved;
    saved.flowId = QLatin1String(flows::versus::kFlowId);
    saved.schemaVersion = flows::versus::kStateSchemaVersion;
    saved.payload = payload;

    const VersusFlow flow;
    const domain::RestoreResult result = flow.restore(saved);
    QVERIFY(!result.restored);
    QVERIFY(result.message.contains(QStringLiteral("not internally consistent")));
}

void TestVersusFlow::finishingEarlyMarksOnlyDecidedLosers() {
    const VersusFlow flow;
    domain::FlowState state = flow.initialise(snapshotOf(8), domain::FlowOptions{});

    // Two decisions, then stop.
    for (int decision = 0; decision < 2; ++decision) {
        const MatchView match = VersusFlow::pendingMatch(state);
        state = flow.reduce(state, eliminate(match.left, state.revision)).state;
    }

    const domain::FlowSummary summary = flow.summarise(state);
    QVERIFY(!summary.complete);
    QVERIFY2(summary.canFinish, "a versus session may always finish early");
    QCOMPARE(summary.draftRejected.size(), 2);
    QCOMPARE(summary.remaining.size(), 6);
}

QTEST_APPLESS_MAIN(TestVersusFlow)
#include "tst_versus_flow.moc"
