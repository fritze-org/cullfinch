// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/wall/WallFlow.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QTest>

using namespace cullfinch;
using cullfinch::flows::wall::LayoutMode;
using cullfinch::flows::wall::WallFlow;

namespace {

domain::SelectionSnapshot snapshotOf(int count) {
    domain::SelectionSnapshot snapshot;
    snapshot.collectionId = domain::CollectionId(QStringLiteral("c1"));
    for (int index = 0; index < count; ++index) {
        const domain::AssetId id(QStringLiteral("asset-%1").arg(index));
        snapshot.orderedAssetIds.append(id);
        snapshot.membershipRevisions.insert(id, 1);
    }
    return snapshot;
}

domain::FlowAction action(const QString& name, const QJsonObject& payload, quint64 revision) {
    domain::FlowAction result;
    result.flowId = QLatin1String(flows::wall::kFlowId);
    result.name = name;
    result.payload = payload;
    result.expectedStateRevision = revision;
    return result;
}

domain::FlowAction eliminate(const domain::AssetId& id, quint64 revision) {
    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), id.toString());
    return action(QLatin1String(flows::wall::kActionEliminate), payload, revision);
}

} // namespace

class TestWallFlow : public QObject {
    Q_OBJECT

private slots:
    void showsEverySelectedCandidate();
    void eliminatingRemovesOnlyTheNamedPhoto();
    void reflowPreservesRelativeOrder();
    void fixedPositionsLeaveAPlaceholderUntilCompact();
    void aPlaceholderKeepsTheEliminatedCandidate();
    void refusesASecondEliminationOfAHeldPlaceholder();
    void restoresADraftWhosePlaceholdersAreAnonymous();
    void switchingBackToReflowCompacts();
    void finishesWithAnyNumberOfSurvivorsIncludingZero();
    void refusesARepeatEliminationOfAVanishedTile();
    void remainingAndRejectedPartitionTheInput();
    void restoreRejectsAnotherFlowsState();
};

void TestWallFlow::showsEverySelectedCandidate() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(37);
    const domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    // The wall never silently paginates or omits a candidate.
    QCOMPARE(WallFlow::candidates(state), selection.orderedAssetIds);
    QCOMPARE(flow.summarise(state).remaining.size(), 37);
}

void TestWallFlow::eliminatingRemovesOnlyTheNamedPhoto() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(5);
    const domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    const domain::AssetId target = selection.orderedAssetIds.at(2);
    const domain::TransitionResult result = flow.reduce(state, eliminate(target, state.revision));
    QVERIFY(result.accepted);
    QCOMPARE(result.delta.rejected, QList<domain::AssetId>{target});

    const QList<domain::AssetId> remaining = WallFlow::candidates(result.state);
    QCOMPARE(remaining.size(), 4);
    QVERIFY(!remaining.contains(target));
}

void TestWallFlow::reflowPreservesRelativeOrder() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(6);
    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});
    QCOMPARE(WallFlow::layoutMode(state), LayoutMode::Reflow);

    state = flow.reduce(state, eliminate(selection.orderedAssetIds.at(1), state.revision)).state;
    state = flow.reduce(state, eliminate(selection.orderedAssetIds.at(4), state.revision)).state;

    const QList<domain::AssetId> expected{
        selection.orderedAssetIds.at(0), selection.orderedAssetIds.at(2),
        selection.orderedAssetIds.at(3), selection.orderedAssetIds.at(5)};
    QCOMPARE(WallFlow::candidates(state), expected);
    QVERIFY(!WallFlow::hasPlaceholders(state));
}

void TestWallFlow::fixedPositionsLeaveAPlaceholderUntilCompact() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(4);
    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    QJsonObject mode;
    mode.insert(QStringLiteral("mode"), QStringLiteral("fixed"));
    state = flow.reduce(state, action(QLatin1String(flows::wall::kActionSetLayoutMode), mode,
                                      state.revision))
                .state;
    QCOMPARE(WallFlow::layoutMode(state), LayoutMode::FixedPositions);

    state = flow.reduce(state, eliminate(selection.orderedAssetIds.at(1), state.revision)).state;

    // The survivors keep their positions, so spatial memory survives.
    const QList<flows::wall::WallSlot> positions = WallFlow::positions(state);
    QCOMPARE(positions.size(), 4);
    QVERIFY(positions.at(1).isPlaceholder());
    QCOMPARE(positions.at(2).id, selection.orderedAssetIds.at(2));
    QVERIFY(WallFlow::hasPlaceholders(state));

    state = flow.reduce(state, action(QLatin1String(flows::wall::kActionCompact), QJsonObject{},
                                      state.revision))
                .state;
    QCOMPARE(WallFlow::positions(state).size(), 3);
    QVERIFY(!WallFlow::hasPlaceholders(state));

    // Compacting again has nothing to do and is refused rather than producing
    // an empty state transition.
    QVERIFY(!flow.reduce(state, action(QLatin1String(flows::wall::kActionCompact), QJsonObject{},
                                       state.revision))
                 .accepted);
}

void TestWallFlow::aPlaceholderKeepsTheEliminatedCandidate() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(4);
    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    QJsonObject mode;
    mode.insert(QStringLiteral("mode"), QStringLiteral("fixed"));
    state = flow.reduce(state, action(QLatin1String(flows::wall::kActionSetLayoutMode), mode,
                                      state.revision))
                .state;

    const domain::AssetId target = selection.orderedAssetIds.at(1);
    state = flow.reduce(state, eliminate(target, state.revision)).state;

    // The placeholder names the photo whose cell it is holding, which is what
    // lets the wall leave that photo in place marked as eliminated instead of
    // showing an anonymous gap.
    const flows::wall::WallSlot held = WallFlow::positions(state).at(1);
    QVERIFY(held.isPlaceholder());
    QVERIFY(held.rejected);
    QCOMPARE(held.id, target);

    // It is still not a survivor, and the identity survives a save and reload.
    QVERIFY(!WallFlow::candidates(state).contains(target));
    QVERIFY(!flow.summarise(state).remaining.contains(target));

    domain::VersionedFlowState saved;
    saved.flowId = state.flowId;
    saved.schemaVersion = state.schemaVersion;
    saved.revision = state.revision;
    saved.payload = state.payload;
    const domain::RestoreResult restored = flow.restore(saved);
    QVERIFY(restored.restored);
    QCOMPARE(WallFlow::positions(restored.state).at(1).id, target);
    QVERIFY(WallFlow::positions(restored.state).at(1).rejected);
}

void TestWallFlow::refusesASecondEliminationOfAHeldPlaceholder() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(3);
    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    QJsonObject mode;
    mode.insert(QStringLiteral("mode"), QStringLiteral("fixed"));
    state = flow.reduce(state, action(QLatin1String(flows::wall::kActionSetLayoutMode), mode,
                                      state.revision))
                .state;

    const domain::AssetId target = selection.orderedAssetIds.first();
    const domain::FlowState after = flow.reduce(state, eliminate(target, state.revision)).state;

    // Its tile is still on the wall, so a second gesture can reach it. One
    // photo is one decision: the repeat is refused rather than duplicated.
    QVERIFY(!flow.reduce(after, eliminate(target, after.revision)).accepted);
    QCOMPARE(flow.summarise(after).draftRejected.size(), 1);
}

void TestWallFlow::restoresADraftWhosePlaceholdersAreAnonymous() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(3);

    // A draft written before placeholders kept their candidate: the cell is a
    // bare null. It still restores, and the cell is still held -- there is
    // simply no photo to show in it.
    QJsonArray positions;
    positions.append(selection.orderedAssetIds.at(0).toString());
    positions.append(QJsonValue(QJsonValue::Null));
    positions.append(selection.orderedAssetIds.at(2).toString());

    QJsonObject payload;
    payload.insert(QStringLiteral("slots"), positions);
    payload.insert(QStringLiteral("rejected"),
                   QJsonArray{selection.orderedAssetIds.at(1).toString()});
    payload.insert(QStringLiteral("input"), domain::toJsonArray(selection.orderedAssetIds));
    payload.insert(QStringLiteral("layoutMode"), QStringLiteral("fixed"));

    domain::VersionedFlowState saved;
    saved.flowId = QLatin1String(flows::wall::kFlowId);
    saved.schemaVersion = flows::wall::kStateSchemaVersion;
    saved.revision = 7;
    saved.payload = payload;

    const domain::RestoreResult restored = flow.restore(saved);
    QVERIFY(restored.restored);
    QCOMPARE(WallFlow::positions(restored.state).size(), 3);
    QVERIFY(WallFlow::positions(restored.state).at(1).isPlaceholder());
    QVERIFY(!WallFlow::positions(restored.state).at(1).id.isValid());
    QVERIFY(WallFlow::hasPlaceholders(restored.state));
    QCOMPARE(WallFlow::candidates(restored.state).size(), 2);
}

void TestWallFlow::switchingBackToReflowCompacts() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(3);
    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    QJsonObject fixed;
    fixed.insert(QStringLiteral("mode"), QStringLiteral("fixed"));
    state = flow.reduce(state, action(QLatin1String(flows::wall::kActionSetLayoutMode), fixed,
                                      state.revision))
                .state;
    state = flow.reduce(state, eliminate(selection.orderedAssetIds.at(0), state.revision)).state;
    QVERIFY(WallFlow::hasPlaceholders(state));

    QJsonObject reflow;
    reflow.insert(QStringLiteral("mode"), QStringLiteral("reflow"));
    state = flow.reduce(state, action(QLatin1String(flows::wall::kActionSetLayoutMode), reflow,
                                      state.revision))
                .state;
    QVERIFY(!WallFlow::hasPlaceholders(state));
    QCOMPARE(WallFlow::candidates(state).size(), 2);
}

void TestWallFlow::finishesWithAnyNumberOfSurvivorsIncludingZero() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(3);
    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});

    // Unlike versus, the wall may finish at any point, and the empty wall is a
    // legitimate final state.
    QVERIFY(flow.summarise(state).canFinish);
    for (const domain::AssetId& id : selection.orderedAssetIds) {
        state = flow.reduce(state, eliminate(id, state.revision)).state;
        QVERIFY(flow.summarise(state).canFinish);
    }

    const domain::FlowSummary summary = flow.summarise(state);
    QCOMPARE(summary.remaining.size(), 0);
    QCOMPARE(summary.draftRejected.size(), 3);
    QVERIFY(summary.canFinish);
}

void TestWallFlow::refusesARepeatEliminationOfAVanishedTile() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(3);
    const domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});
    const domain::AssetId target = selection.orderedAssetIds.first();

    const domain::FlowState after = flow.reduce(state, eliminate(target, state.revision)).state;
    const domain::TransitionResult repeat = flow.reduce(after, eliminate(target, after.revision));

    QVERIFY(!repeat.accepted);
    QCOMPARE(flow.summarise(after).draftRejected.size(), 1);
}

void TestWallFlow::remainingAndRejectedPartitionTheInput() {
    const WallFlow flow;
    const domain::SelectionSnapshot selection = snapshotOf(7);
    const QSet<domain::AssetId> input(selection.orderedAssetIds.cbegin(),
                                      selection.orderedAssetIds.cend());

    domain::FlowState state = flow.initialise(selection, domain::FlowOptions{});
    for (const domain::AssetId& id : selection.orderedAssetIds) {
        const domain::FlowSummary summary = flow.summarise(state);
        const QSet<domain::AssetId> remaining(summary.remaining.cbegin(), summary.remaining.cend());
        const QSet<domain::AssetId> rejected(summary.draftRejected.cbegin(),
                                             summary.draftRejected.cend());
        QVERIFY(!remaining.intersects(rejected));
        QCOMPARE(remaining | rejected, input);
        state = flow.reduce(state, eliminate(id, state.revision)).state;
    }
}

void TestWallFlow::restoreRejectsAnotherFlowsState() {
    const WallFlow flow;
    domain::VersionedFlowState saved;
    saved.flowId = QStringLiteral("versus-tree");
    saved.schemaVersion = flows::wall::kStateSchemaVersion;
    QVERIFY(!flow.restore(saved).restored);

    saved.flowId = QLatin1String(flows::wall::kFlowId);
    saved.schemaVersion = flows::wall::kStateSchemaVersion + 1;
    QVERIFY(!flow.restore(saved).restored);
}

QTEST_APPLESS_MAIN(TestWallFlow)
#include "tst_wall_flow.moc"
