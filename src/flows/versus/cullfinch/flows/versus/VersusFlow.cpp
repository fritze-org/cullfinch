// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/versus/VersusFlow.h>

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <algorithm>
#include <utility>

namespace cullfinch::flows::versus {
namespace {

using domain::DecisionDelta;
using domain::FlowAction;
using domain::FlowDescriptor;
using domain::FlowState;
using domain::FlowSummary;
using domain::RestoreResult;
using domain::SelectionSnapshot;
using domain::TransitionResult;
using domain::ValidationResult;
using domain::VersionedFlowState;

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

constexpr auto kKeyBracketSize = "bracketSize";
constexpr auto kKeySlots = "slots";
constexpr auto kKeyDecisions = "decisions";
constexpr auto kKeyInput = "input";
constexpr auto kKeyNode = "node";
constexpr auto kKeyEliminated = "eliminated";
constexpr auto kKeyAssetId = "assetId";

int nextPowerOfTwo(int value) {
    int size = 1;
    while (size < value) {
        size *= 2;
    }
    return size;
}

/// Resolved bracket contents parsed out of the persisted payload.
struct Bracket {
    bool valid = false;
    int bracketSize = 0;
    QList<AssetId> positions;      ///< Length bracketSize; invalid entries are byes.
    QList<AssetId> input;          ///< Frozen selection order.
    QHash<int, AssetId> decisions; ///< node -> eliminated candidate.
    QList<int> decisionOrder;

    [[nodiscard]] int internalNodeCount() const { return bracketSize - 1; }
    [[nodiscard]] bool isLeaf(int node) const { return node >= internalNodeCount(); }
    [[nodiscard]] int slotOfLeaf(int node) const { return node - internalNodeCount(); }
};

Bracket parse(const FlowState& state) {
    Bracket bracket;
    const QJsonObject& payload = state.payload;

    const int bracketSize = payload.value(QLatin1String(kKeyBracketSize)).toInt(0);
    const QJsonArray positions = payload.value(QLatin1String(kKeySlots)).toArray();
    if (bracketSize <= 0 || positions.size() != bracketSize) {
        return bracket;
    }

    bracket.bracketSize = bracketSize;
    bracket.positions.reserve(bracketSize);
    for (const auto& value : positions) {
        bracket.positions.append(value.isString() ? AssetId(value.toString()) : AssetId());
    }

    bracket.input = domain::assetIdsFromJson(payload.value(QLatin1String(kKeyInput)).toArray());

    const QJsonArray decisions = payload.value(QLatin1String(kKeyDecisions)).toArray();
    for (const auto& value : decisions) {
        const QJsonObject decision = value.toObject();
        const int node = decision.value(QLatin1String(kKeyNode)).toInt(-1);
        const AssetId eliminated(decision.value(QLatin1String(kKeyEliminated)).toString());
        if (node < 0 || node >= bracket.internalNodeCount() || !eliminated.isValid()) {
            return Bracket{};
        }
        if (bracket.decisions.contains(node)) {
            return Bracket{};
        }
        bracket.decisions.insert(node, eliminated);
        bracket.decisionOrder.append(node);
    }

    bracket.valid = true;
    return bracket;
}

QJsonObject serialise(const Bracket& bracket) {
    QJsonArray positions;
    for (const AssetId& id : bracket.positions) {
        if (id.isValid()) {
            positions.append(id.toString());
        } else {
            positions.append(QJsonValue(QJsonValue::Null));
        }
    }

    QJsonArray decisions;
    for (int node : bracket.decisionOrder) {
        QJsonObject decision;
        decision.insert(QLatin1String(kKeyNode), node);
        decision.insert(QLatin1String(kKeyEliminated), bracket.decisions.value(node).toString());
        decisions.append(decision);
    }

    QJsonObject payload;
    payload.insert(QLatin1String(kKeyBracketSize), bracket.bracketSize);
    payload.insert(QLatin1String(kKeySlots), positions);
    payload.insert(QLatin1String(kKeyInput), domain::toJsonArray(bracket.input));
    payload.insert(QLatin1String(kKeyDecisions), decisions);
    return payload;
}

/// The candidate that comes out of a subtree.
///
/// `resolved == false` means the subtree still needs a decision.
/// `resolved == true` with an invalid id means nothing advanced through it,
/// which is how a bye is represented without inventing a fake opponent.
struct Outcome {
    bool resolved = false;
    AssetId id;
};

Outcome resolveNode(const Bracket& bracket, int node) {
    if (bracket.isLeaf(node)) {
        return Outcome{true, bracket.positions.at(bracket.slotOfLeaf(node))};
    }

    const Outcome left = resolveNode(bracket, (2 * node) + 1);
    const Outcome right = resolveNode(bracket, (2 * node) + 2);
    if (!left.resolved || !right.resolved) {
        return Outcome{};
    }
    if (!left.id.isValid()) {
        return Outcome{true, right.id};
    }
    if (!right.id.isValid()) {
        return Outcome{true, left.id};
    }

    const auto decision = bracket.decisions.constFind(node);
    if (decision == bracket.decisions.constEnd()) {
        return Outcome{};
    }
    if (*decision == left.id) {
        return Outcome{true, right.id};
    }
    if (*decision == right.id) {
        return Outcome{true, left.id};
    }
    // A decision naming neither candidate cannot be honoured. Treat the subtree
    // as unresolved rather than silently advancing an arbitrary candidate.
    return Outcome{};
}

int totalRoundsFor(int bracketSize) {
    // Shifts are done on an unsigned value: a bracket size is a count, and
    // shifting a signed operand mixes signedness for no benefit.
    int rounds = 0;
    while (static_cast<int>(1U << static_cast<unsigned>(rounds)) < bracketSize) {
        ++rounds;
    }
    return rounds;
}

/// Ready matches are scheduled by round, then by bracket position: deepest
/// level first, ascending index within the level.
MatchView findPending(const Bracket& bracket) {
    MatchView pending;
    if (!bracket.valid || bracket.bracketSize < 2) {
        return pending;
    }
    const int rounds = totalRoundsFor(bracket.bracketSize);
    pending.totalRounds = rounds;

    for (int depth = rounds - 1; depth >= 0; --depth) {
        const int first = static_cast<int>(1U << static_cast<unsigned>(depth)) - 1;
        const int last = static_cast<int>(1U << static_cast<unsigned>(depth + 1)) - 2;
        for (int node = first; node <= last; ++node) {
            if (node >= bracket.internalNodeCount() || bracket.decisions.contains(node)) {
                continue;
            }
            const Outcome left = resolveNode(bracket, (2 * node) + 1);
            const Outcome right = resolveNode(bracket, (2 * node) + 2);
            if (!left.resolved || !right.resolved) {
                continue;
            }
            if (!left.id.isValid() || !right.id.isValid()) {
                continue; // A bye: advances without a decision.
            }
            pending.node = node;
            pending.round = rounds - depth;
            pending.positionInRound = node - first;
            pending.left = left.id;
            pending.right = right.id;
            return pending;
        }
    }
    return pending;
}

QList<AssetId> eliminatedCandidates(const Bracket& bracket) {
    QList<AssetId> rejected;
    rejected.reserve(bracket.decisionOrder.size());
    for (int node : bracket.decisionOrder) {
        rejected.append(bracket.decisions.value(node));
    }
    return rejected;
}

} // namespace

FlowDescriptor VersusFlow::descriptor() const {
    FlowDescriptor descriptor;
    descriptor.id = QLatin1String(kFlowId);
    descriptor.displayName = tr("Versus tree");
    descriptor.description =
        tr("Compare two photos at a time in a balanced bracket until one remains.");
    descriptor.stateSchemaVersion = kStateSchemaVersion;
    descriptor.minimumInputSize = 1;
    descriptor.maximumInputSize = -1;
    descriptor.supportedActions = QStringList{QLatin1String(kActionEliminate)};
    return descriptor;
}

ValidationResult VersusFlow::validate(const SelectionSnapshot& selection,
                                      const domain::FlowOptions& options) const {
    Q_UNUSED(options)
    if (selection.isEmpty()) {
        return ValidationResult::failure(
            tr("No eligible photos are selected, so there is nothing to compare."));
    }
    return ValidationResult::ok();
}

FlowState VersusFlow::initialise(const SelectionSnapshot& selection,
                                 const domain::FlowOptions& options) const {
    Q_UNUSED(options)

    Bracket bracket;
    bracket.valid = true;
    bracket.input = selection.orderedAssetIds;
    const int candidates = static_cast<int>(selection.orderedAssetIds.size());
    bracket.bracketSize = nextPowerOfTwo(std::max(candidates, 1));

    // Standard seeding: candidate i takes the bracket position holding seed
    // i+1. Positions whose seed exceeds the candidate count stay empty, which
    // distributes the first-round byes across the bracket.
    const QList<int> order = seedOrder(bracket.bracketSize);
    bracket.positions.resize(bracket.bracketSize);
    for (int position = 0; position < bracket.bracketSize; ++position) {
        const int seed = order.at(position);
        if (seed <= candidates) {
            bracket.positions[position] = selection.orderedAssetIds.at(seed - 1);
        }
    }

    FlowState state;
    state.flowId = QLatin1String(kFlowId);
    state.schemaVersion = kStateSchemaVersion;
    state.revision = 1;
    state.payload = serialise(bracket);
    return state;
}

TransitionResult VersusFlow::reduce(const FlowState& state, const FlowAction& action) const {
    if (action.flowId != QLatin1String(kFlowId)) {
        return TransitionResult::rejectedWith(
            tr("Action '%1' does not belong to the versus flow.").arg(action.name));
    }
    if (action.name != QLatin1String(kActionEliminate)) {
        return TransitionResult::rejectedWith(tr("Unsupported action '%1'.").arg(action.name));
    }

    Bracket bracket = parse(state);
    if (!bracket.valid) {
        return TransitionResult::rejectedWith(tr("The saved bracket could not be read."));
    }

    const MatchView pending = findPending(bracket);
    if (!pending.isValid()) {
        // A late click or a repeated key against a completed match must not
        // decide a second match.
        return TransitionResult::rejectedWith(tr("This match has already been decided."));
    }

    const AssetId eliminated(action.payload.value(QLatin1String(kKeyAssetId)).toString());
    if (!eliminated.isValid()) {
        return TransitionResult::rejectedWith(tr("The action did not name a candidate."));
    }
    if (!(eliminated == pending.left) && !(eliminated == pending.right)) {
        return TransitionResult::rejectedWith(
            tr("'%1' is not one of the two candidates in the current match.")
                .arg(eliminated.toString()));
    }

    bracket.decisions.insert(pending.node, eliminated);
    bracket.decisionOrder.append(pending.node);

    FlowState next;
    next.flowId = state.flowId;
    next.schemaVersion = state.schemaVersion;
    next.revision = state.revision + 1;
    next.payload = serialise(bracket);

    DecisionDelta delta;
    delta.rejected.append(eliminated);
    return TransitionResult::accept(next, delta);
}

FlowSummary VersusFlow::summarise(const FlowState& state) const {
    FlowSummary summary;
    const Bracket bracket = parse(state);
    if (!bracket.valid) {
        summary.statusText = tr("The saved bracket could not be read.");
        return summary;
    }

    summary.draftRejected = eliminatedCandidates(bracket);
    const QSet<AssetId> rejected(summary.draftRejected.cbegin(), summary.draftRejected.cend());
    for (const AssetId& id : bracket.input) {
        if (!rejected.contains(id)) {
            summary.remaining.append(id);
        }
    }

    summary.decisionsMade = static_cast<int>(bracket.decisionOrder.size());
    summary.decisionsExpected = std::max(0, static_cast<int>(bracket.input.size()) - 1);

    const Outcome root = bracket.bracketSize >= 2 ? resolveNode(bracket, 0)
                                                  : Outcome{true, bracket.positions.value(0)};
    summary.complete = root.resolved;
    // Versus may always finish early: only decided losers become marks.
    summary.canFinish = true;

    if (summary.complete) {
        summary.statusText = tr("%1 remaining, %2 eliminated")
                                 .arg(summary.remaining.size())
                                 .arg(summary.draftRejected.size());
    } else {
        summary.statusText =
            tr("Decision %1 of %2").arg(summary.decisionsMade + 1).arg(summary.decisionsExpected);
    }
    return summary;
}

RestoreResult VersusFlow::restore(const VersionedFlowState& saved) const {
    if (saved.flowId != QLatin1String(kFlowId)) {
        return RestoreResult::failure(
            tr("Saved state belongs to flow '%1', not the versus tree.").arg(saved.flowId));
    }
    if (saved.schemaVersion != kStateSchemaVersion) {
        // An unknown or newer state version must never be interpreted. The
        // record is preserved and reported as incompatible instead.
        return RestoreResult::failure(
            tr("Saved versus state uses schema version %1; this build supports version %2.")
                .arg(saved.schemaVersion)
                .arg(kStateSchemaVersion));
    }

    FlowState state;
    state.flowId = saved.flowId;
    state.schemaVersion = saved.schemaVersion;
    state.revision = saved.revision;
    state.payload = saved.payload;

    if (!parse(state).valid) {
        return RestoreResult::failure(tr("The saved bracket is not internally consistent."));
    }

    RestoreResult result;
    result.restored = true;
    result.state = std::move(state);
    return result;
}

MatchView VersusFlow::pendingMatch(const FlowState& state) {
    return findPending(parse(state));
}

AssetId VersusFlow::survivor(const FlowState& state) {
    const Bracket bracket = parse(state);
    if (!bracket.valid) {
        return AssetId();
    }
    if (bracket.bracketSize < 2) {
        return bracket.positions.value(0);
    }
    const Outcome root = resolveNode(bracket, 0);
    return root.resolved ? root.id : AssetId();
}

QList<AssetId> VersusFlow::bracketSlots(const FlowState& state) {
    return parse(state).positions;
}

QList<int> VersusFlow::seedOrder(int bracketSize) {
    QList<int> order{1};
    while (order.size() < bracketSize) {
        const int total = static_cast<int>(order.size()) * 2;
        QList<int> expanded;
        expanded.reserve(total);
        for (int seed : order) {
            expanded.append(seed);
            expanded.append(total + 1 - seed);
        }
        order = expanded;
    }
    return order;
}

} // namespace cullfinch::flows::versus
