// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/Ids.h>
#include <cullfinch/domain/SelectionSnapshot.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <utility>

namespace cullfinch::domain {

/// Static description of a comparison flow.
struct FlowDescriptor {
    QString id;          ///< Stable string identifier; persisted with sessions.
    QString displayName; ///< Shown in the comparison menu.
    QString description;
    int stateSchemaVersion = 1;
    int minimumInputSize = 1;
    int maximumInputSize = -1; ///< -1 means unbounded.
    QStringList supportedActions;

    [[nodiscard]] bool isValid() const { return !id.isEmpty() && stateSchemaVersion > 0; }
};

/// Per-session flow configuration, decoded by the flow itself.
struct FlowOptions {
    QJsonObject values;

    [[nodiscard]] bool boolean(const QString& key, bool fallback) const;
    [[nodiscard]] QString string(const QString& key, const QString& fallback) const;
    [[nodiscard]] int integer(const QString& key, int fallback) const;
};

struct ValidationResult {
    bool valid = false;
    QString message;

    [[nodiscard]] static ValidationResult ok() { return ValidationResult{true, QString()}; }
    [[nodiscard]] static ValidationResult failure(QString message) {
        return ValidationResult{false, std::move(message)};
    }
};

/// Flow-owned state. The payload schema belongs to the flow: the host stores
/// and versions it but never interprets it. This is what keeps a shared
/// `variant<VersusState, WallState, ...>` out of the application layer.
struct FlowState {
    QString flowId;
    int schemaVersion = 0;
    quint64 revision = 0;
    QJsonObject payload;

    [[nodiscard]] bool isValid() const { return !flowId.isEmpty() && schemaVersion > 0; }
};

/// A persisted state, as read back from storage. Its schema version may be
/// unknown or newer than this build supports.
struct VersionedFlowState {
    QString flowId;
    int schemaVersion = 0;
    quint64 revision = 0;
    QJsonObject payload;
};

/// A versioned action envelope. The name and payload are flow-local; each flow
/// decodes them into its own typed action.
struct FlowAction {
    SessionId sessionId;
    QString flowId;
    QString name;
    QJsonObject payload;
    quint64 expectedStateRevision = 0;
};

/// The reversible decision change produced by a transition.
///
/// `rejected` are added to the session draft, `restored` removed from it. The
/// host validates that every identifier belongs to the frozen input.
struct DecisionDelta {
    QList<AssetId> rejected;
    QList<AssetId> restored;

    [[nodiscard]] bool isEmpty() const { return rejected.isEmpty() && restored.isEmpty(); }
    [[nodiscard]] DecisionDelta inverted() const { return DecisionDelta{restored, rejected}; }
};

struct TransitionResult {
    bool accepted = false;
    QString message;
    FlowState state;
    DecisionDelta delta;

    [[nodiscard]] static TransitionResult rejectedWith(QString message) {
        TransitionResult result;
        result.message = std::move(message);
        return result;
    }
    [[nodiscard]] static TransitionResult accept(FlowState state, DecisionDelta delta) {
        TransitionResult result;
        result.accepted = true;
        result.state = std::move(state);
        result.delta = std::move(delta);
        return result;
    }
};

/// What the shared comparison shell needs to know about any flow.
struct FlowSummary {
    QList<AssetId> remaining;
    QList<AssetId> draftRejected;
    int decisionsMade = 0;
    int decisionsExpected = -1; ///< -1 when the flow cannot predict a total.
    bool complete = false;
    /// Completion rules belong to the flow. Versus finishes at one survivor;
    /// the wall may finish with any number, including zero. The shared session
    /// controller must not hardcode either.
    bool canFinish = false;
    QString statusText;
};

struct RestoreResult {
    bool restored = false;
    QString message;
    FlowState state;

    [[nodiscard]] static RestoreResult failure(QString message) {
        RestoreResult result;
        result.message = std::move(message);
        return result;
    }
};

/// The identity and schema checks every flow's restore() repeats, plus the
/// assembly of the persisted fields into a fresh FlowState. A flow still
/// validates its own payload afterwards: this only rules out a state that
/// does not belong to it or that a newer build wrote.
[[nodiscard]] RestoreResult restoreFlowState(const VersionedFlowState& saved, const QString& flowId,
                                             int stateSchemaVersion,
                                             const QString& wrongFlowMessage,
                                             const QString& wrongSchemaMessage);

/// A comparison flow: a pure, deterministic reducer over its own state.
///
/// Implementations receive asset identifiers and display metadata, never
/// writable filesystem handles (invariant 2). Transitions perform no I/O and
/// consult no clock or random source, which is what makes undo, resume and
/// replay exact.
class IComparisonFlow {
public:
    IComparisonFlow() = default;
    virtual ~IComparisonFlow() = default;
    IComparisonFlow(const IComparisonFlow&) = delete;
    IComparisonFlow& operator=(const IComparisonFlow&) = delete;
    IComparisonFlow(IComparisonFlow&&) = delete;
    IComparisonFlow& operator=(IComparisonFlow&&) = delete;

    [[nodiscard]] virtual FlowDescriptor descriptor() const = 0;

    [[nodiscard]] virtual ValidationResult validate(const SelectionSnapshot& selection,
                                                    const FlowOptions& options) const = 0;

    [[nodiscard]] virtual FlowState initialise(const SelectionSnapshot& selection,
                                               const FlowOptions& options) const = 0;

    [[nodiscard]] virtual TransitionResult reduce(const FlowState& state,
                                                  const FlowAction& action) const = 0;

    [[nodiscard]] virtual FlowSummary summarise(const FlowState& state) const = 0;

    [[nodiscard]] virtual RestoreResult restore(const VersionedFlowState& saved) const = 0;
};

/// Shared helpers for encoding identifier lists in flow payloads.
[[nodiscard]] QJsonArray toJsonArray(const QList<AssetId>& ids);
[[nodiscard]] QList<AssetId> assetIdsFromJson(const QJsonArray& array);

} // namespace cullfinch::domain
