// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/flows/wall/WallFlow.h>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <algorithm>

namespace cullfinch::flows::wall {
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

constexpr auto kKeySlots = "slots";
constexpr auto kKeyRejected = "rejected";
constexpr auto kKeyInput = "input";
constexpr auto kKeyLayoutMode = "layoutMode";
constexpr auto kKeyAssetId = "assetId";
constexpr auto kKeyMode = "mode";

struct Wall {
    bool valid = false;
    QList<WallSlot> positions;
    QList<AssetId> rejected;
    QList<AssetId> input;
    LayoutMode mode = LayoutMode::Reflow;
};

Wall parse(const FlowState& state) {
    Wall wall;
    const QJsonObject& payload = state.payload;
    if (!payload.contains(QLatin1String(kKeySlots)) ||
        !payload.contains(QLatin1String(kKeyInput))) {
        return wall;
    }

    const QJsonArray positions = payload.value(QLatin1String(kKeySlots)).toArray();
    for (const auto& value : positions) {
        if (value.isNull()) {
            // A draft written before placeholders kept their candidate. The
            // cell is held, but nothing is known about what used to fill it.
            wall.positions.append(WallSlot{AssetId{}, true});
        } else if (value.isString()) {
            wall.positions.append(WallSlot{AssetId(value.toString()), false});
        } else if (value.isObject()) {
            const QJsonObject placeholder = value.toObject();
            const AssetId held(placeholder.value(QLatin1String(kKeyAssetId)).toString());
            if (!held.isValid() || !placeholder.value(QLatin1String(kKeyRejected)).toBool()) {
                return Wall{}; // A surviving candidate is a plain identifier.
            }
            wall.positions.append(WallSlot{held, true});
        } else {
            return Wall{};
        }
    }

    wall.rejected = domain::assetIdsFromJson(payload.value(QLatin1String(kKeyRejected)).toArray());
    wall.input = domain::assetIdsFromJson(payload.value(QLatin1String(kKeyInput)).toArray());
    wall.mode = layoutModeFromToken(payload.value(QLatin1String(kKeyLayoutMode)).toString(),
                                    LayoutMode::Reflow);

    // The cells and the rejection set are read by different halves of the
    // application -- the view paints the cells, the summary reports the set --
    // so a saved wall whose two halves disagree is refused rather than shown.
    // A cell marked eliminated that the draft does not count as eliminated
    // would paint as eliminated while Finish marked nothing.
    const QSet<AssetId> input(wall.input.cbegin(), wall.input.cend());
    const QSet<AssetId> rejected(wall.rejected.cbegin(), wall.rejected.cend());
    QSet<AssetId> named;
    for (const WallSlot& slot : wall.positions) {
        if (!slot.id.isValid()) {
            continue; // An anonymous placeholder names nothing to check.
        }
        if (!input.contains(slot.id) || named.contains(slot.id) ||
            slot.rejected != rejected.contains(slot.id)) {
            return Wall{};
        }
        named.insert(slot.id);
    }

    wall.valid = true;
    return wall;
}

QJsonObject serialise(const Wall& wall) {
    QJsonArray positions;
    for (const WallSlot& slot : wall.positions) {
        if (!slot.isPlaceholder()) {
            positions.append(slot.id.toString());
        } else if (slot.id.isValid()) {
            // The schema version is deliberately unchanged: this is additive,
            // every older payload still reads, and bumping it would make every
            // paused wall session unresumable for the sake of one extra field.
            QJsonObject placeholder;
            placeholder.insert(QLatin1String(kKeyAssetId), slot.id.toString());
            placeholder.insert(QLatin1String(kKeyRejected), true);
            positions.append(placeholder);
        } else {
            positions.append(QJsonValue(QJsonValue::Null));
        }
    }

    QJsonObject payload;
    payload.insert(QLatin1String(kKeySlots), positions);
    payload.insert(QLatin1String(kKeyRejected), domain::toJsonArray(wall.rejected));
    payload.insert(QLatin1String(kKeyInput), domain::toJsonArray(wall.input));
    payload.insert(QLatin1String(kKeyLayoutMode), layoutModeToken(wall.mode));
    return payload;
}

FlowState advance(const FlowState& previous, const Wall& wall) {
    FlowState next;
    next.flowId = previous.flowId;
    next.schemaVersion = previous.schemaVersion;
    next.revision = previous.revision + 1;
    next.payload = serialise(wall);
    return next;
}

} // namespace

QString layoutModeToken(LayoutMode mode) {
    return mode == LayoutMode::FixedPositions ? QStringLiteral("fixed") : QStringLiteral("reflow");
}

LayoutMode layoutModeFromToken(const QString& token, LayoutMode fallback) {
    if (token == QLatin1String("reflow")) {
        return LayoutMode::Reflow;
    }
    if (token == QLatin1String("fixed")) {
        return LayoutMode::FixedPositions;
    }
    return fallback;
}

FlowDescriptor WallFlow::descriptor() const {
    FlowDescriptor descriptor;
    descriptor.id = QLatin1String(kFlowId);
    descriptor.displayName = tr("Image wall");
    descriptor.description =
        tr("Show every selected photo at once and click the ones to eliminate.");
    descriptor.stateSchemaVersion = kStateSchemaVersion;
    descriptor.minimumInputSize = 1;
    descriptor.maximumInputSize = -1;
    descriptor.supportedActions =
        QStringList{QLatin1String(kActionEliminate), QLatin1String(kActionSetLayoutMode),
                    QLatin1String(kActionCompact)};
    return descriptor;
}

ValidationResult WallFlow::validate(const SelectionSnapshot& selection,
                                    const domain::FlowOptions& options) const {
    Q_UNUSED(options)
    if (selection.isEmpty()) {
        return ValidationResult::failure(
            tr("No eligible photos are selected, so there is nothing to show on the wall."));
    }
    return ValidationResult::ok();
}

FlowState WallFlow::initialise(const SelectionSnapshot& selection,
                               const domain::FlowOptions& options) const {
    Wall wall;
    wall.valid = true;
    wall.input = selection.orderedAssetIds;
    wall.mode =
        layoutModeFromToken(options.string(QLatin1String(kKeyLayoutMode), QStringLiteral("reflow")),
                            LayoutMode::Reflow);
    for (const AssetId& id : selection.orderedAssetIds) {
        wall.positions.append(WallSlot{id});
    }

    FlowState state;
    state.flowId = QLatin1String(kFlowId);
    state.schemaVersion = kStateSchemaVersion;
    state.revision = 1;
    state.payload = serialise(wall);
    return state;
}

TransitionResult WallFlow::reduce(const FlowState& state, const FlowAction& action) const {
    if (action.flowId != QLatin1String(kFlowId)) {
        return TransitionResult::rejectedWith(
            tr("Action '%1' does not belong to the image wall.").arg(action.name));
    }

    Wall wall = parse(state);
    if (!wall.valid) {
        return TransitionResult::rejectedWith(tr("The saved wall could not be read."));
    }

    if (action.name == QLatin1String(kActionEliminate)) {
        const AssetId target(action.payload.value(QLatin1String(kKeyAssetId)).toString());
        if (!target.isValid()) {
            return TransitionResult::rejectedWith(tr("The action did not name a candidate."));
        }

        int position = -1;
        for (int index = 0; index < wall.positions.size(); ++index) {
            const WallSlot& slot = wall.positions.at(index);
            // A placeholder is skipped rather than matched: in fixed-position
            // mode its tile is still on screen, and a second gesture on an
            // already eliminated photo must not duplicate the rejection.
            if (!slot.isPlaceholder() && slot.id == target) {
                position = index;
                break;
            }
        }
        if (position < 0) {
            // A repeat click on a vanished tile, or a gesture that no longer
            // refers to an eligible item, decides nothing.
            return TransitionResult::rejectedWith(tr("That photo is no longer on the wall."));
        }

        if (wall.mode == LayoutMode::FixedPositions) {
            wall.positions[position] = WallSlot{target, true};
        } else {
            wall.positions.removeAt(position);
        }
        wall.rejected.append(target);

        DecisionDelta delta;
        delta.rejected.append(target);
        return TransitionResult::accept(advance(state, wall), delta);
    }

    if (action.name == QLatin1String(kActionSetLayoutMode)) {
        const LayoutMode requested = layoutModeFromToken(
            action.payload.value(QLatin1String(kKeyMode)).toString(), wall.mode);
        if (requested == wall.mode) {
            return TransitionResult::rejectedWith(tr("The wall already uses that layout."));
        }
        wall.mode = requested;
        if (wall.mode == LayoutMode::Reflow) {
            // Switching to reflow implies compacting: placeholders have no
            // meaning once survivors are allowed to move.
            wall.positions.removeIf([](const WallSlot& slot) { return slot.isPlaceholder(); });
        }
        return TransitionResult::accept(advance(state, wall), DecisionDelta{});
    }

    if (action.name == QLatin1String(kActionCompact)) {
        if (wall.positions.removeIf([](const WallSlot& slot) { return slot.isPlaceholder(); }) ==
            0) {
            return TransitionResult::rejectedWith(tr("There is nothing to compact."));
        }
        return TransitionResult::accept(advance(state, wall), DecisionDelta{});
    }

    return TransitionResult::rejectedWith(tr("Unsupported action '%1'.").arg(action.name));
}

FlowSummary WallFlow::summarise(const FlowState& state) const {
    FlowSummary summary;
    const Wall wall = parse(state);
    if (!wall.valid) {
        summary.statusText = tr("The saved wall could not be read.");
        return summary;
    }

    for (const WallSlot& slot : wall.positions) {
        if (!slot.isPlaceholder()) {
            summary.remaining.append(slot.id);
        }
    }
    summary.draftRejected = wall.rejected;
    summary.decisionsMade = static_cast<int>(wall.rejected.size());
    // The wall cannot predict how many decisions a user intends to make.
    summary.decisionsExpected = -1;
    summary.complete = summary.remaining.isEmpty();
    summary.canFinish = true;
    summary.statusText = tr("%1 remaining, %2 eliminated")
                             .arg(summary.remaining.size())
                             .arg(summary.draftRejected.size());
    return summary;
}

RestoreResult WallFlow::restore(const VersionedFlowState& saved) const {
    RestoreResult result = domain::restoreFlowState(
        saved, QLatin1String(kFlowId), kStateSchemaVersion,
        tr("Saved state belongs to flow '%1', not the image wall.").arg(saved.flowId),
        tr("Saved wall state uses schema version %1; this build supports version %2.")
            .arg(saved.schemaVersion)
            .arg(kStateSchemaVersion));
    if (!result.restored) {
        return result;
    }
    if (!parse(result.state).valid) {
        return RestoreResult::failure(tr("The saved wall is not internally consistent."));
    }
    return result;
}

QList<WallSlot> WallFlow::positions(const FlowState& state) {
    return parse(state).positions;
}

QList<AssetId> WallFlow::candidates(const FlowState& state) {
    QList<AssetId> ids;
    for (const WallSlot& slot : parse(state).positions) {
        if (!slot.isPlaceholder()) {
            ids.append(slot.id);
        }
    }
    return ids;
}

LayoutMode WallFlow::layoutMode(const FlowState& state) {
    return parse(state).mode;
}

bool WallFlow::hasPlaceholders(const FlowState& state) {
    return std::ranges::any_of(parse(state).positions,
                               [](const WallSlot& slot) { return slot.isPlaceholder(); });
}

} // namespace cullfinch::flows::wall
