// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FlowContract.h>

#include <QList>
#include <QString>

namespace cullfinch::flows::wall {

using domain::AssetId;

inline constexpr auto kFlowId = "image-wall";
inline constexpr int kStateSchemaVersion = 1;
inline constexpr auto kActionEliminate = "eliminate";
inline constexpr auto kActionSetLayoutMode = "setLayoutMode";
inline constexpr auto kActionCompact = "compact";

/// How the wall reacts to an elimination.
enum class LayoutMode {
    /// Remove the rejected candidate and enlarge the survivors, preserving
    /// relative order. The initial default.
    Reflow,
    /// Leave a placeholder where the candidate was until an explicit Compact,
    /// which preserves spatial memory during rapid culling.
    FixedPositions
};

[[nodiscard]] QString layoutModeToken(LayoutMode mode);
[[nodiscard]] LayoutMode layoutModeFromToken(const QString& token, LayoutMode fallback);

/// One position on the wall. An invalid id is a placeholder left behind in
/// fixed-position mode.
struct WallSlot {
    AssetId id;

    [[nodiscard]] bool isPlaceholder() const { return !id.isValid(); }
};

/// A wall of every selected candidate.
///
/// Completion differs from versus on purpose: finishing may retain any number
/// of survivors, including zero, and the empty wall still offers Undo and
/// Finish. This rule lives here rather than in the shared session controller.
class WallFlow final : public domain::IComparisonFlow {
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

    /// Wall positions in display order, including placeholders.
    [[nodiscard]] static QList<WallSlot> slots(const domain::FlowState& state);

    /// Candidates still on the wall, in order, without placeholders.
    [[nodiscard]] static QList<AssetId> candidates(const domain::FlowState& state);

    [[nodiscard]] static LayoutMode layoutMode(const domain::FlowState& state);

    /// True when at least one placeholder is waiting for Compact.
    [[nodiscard]] static bool hasPlaceholders(const domain::FlowState& state);
};

} // namespace cullfinch::flows::wall
