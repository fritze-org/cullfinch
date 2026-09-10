// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FlowContract.h>

#include <QList>
#include <QString>

namespace cullfinch::flows::versus {

using domain::AssetId;

/// Identifiers for the versus flow, shared with its view.
inline constexpr auto kFlowId = "versus-tree";
inline constexpr int kStateSchemaVersion = 1;
inline constexpr auto kActionEliminate = "eliminate";

/// The two candidates of one match, plus where it sits in the bracket.
struct MatchView {
    int node = -1;
    int round = 0;
    int positionInRound = 0;
    int totalRounds = 0;
    AssetId left;
    AssetId right;

    [[nodiscard]] bool isValid() const { return node >= 0 && left.isValid() && right.isValid(); }
};

/// A deterministic single-elimination bracket over the frozen selection order.
///
/// "Tree" is interpreted as a balanced tournament, not a running champion: a
/// winner is never replayed against every remaining newcomer. The bracket is
/// persisted rather than regenerated, so undo and resume land on exactly the
/// match the user left.
///
/// With N valid candidates, completion takes exactly N-1 elimination decisions
/// and leaves one survivor. Byes advance without showing a fake opponent and
/// without rejecting anything.
class VersusFlow final : public domain::IComparisonFlow {
public:
    [[nodiscard]] domain::FlowDescriptor descriptor() const override;

    [[nodiscard]] domain::ValidationResult
    validate(const domain::SelectionSnapshot& selection,
             const domain::FlowOptions& options) const override;

    [[nodiscard]] domain::FlowState initialise(const domain::SelectionSnapshot& selection,
                                               const domain::FlowOptions& options) const override;

    [[nodiscard]] domain::TransitionResult reduce(const domain::FlowState& state,
                                                  const domain::FlowAction& action) const override;

    [[nodiscard]] domain::FlowSummary summarise(const domain::FlowState& state) const override;

    [[nodiscard]] domain::RestoreResult
    restore(const domain::VersionedFlowState& saved) const override;

    /// The match awaiting a decision, or an invalid MatchView when the bracket
    /// is complete. Used by the view; also the basis of action validation.
    [[nodiscard]] static MatchView pendingMatch(const domain::FlowState& state);

    /// The single remaining candidate once the bracket is complete.
    [[nodiscard]] static AssetId survivor(const domain::FlowState& state);

    /// Bracket positions, including byes, in bracket order. Exposed so the view
    /// and tests can render or assert the actual persisted bracket.
    [[nodiscard]] static QList<AssetId> bracketSlots(const domain::FlowState& state);

    /// Seed order for a bracket of `bracketSize`: element p is the 1-based seed
    /// occupying bracket position p. Byes therefore fall opposite top seeds.
    [[nodiscard]] static QList<int> seedOrder(int bracketSize);
};

} // namespace cullfinch::flows::versus
