// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FlowContract.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace cullfinch::testflows {

/// A minimal third flow that exists only inside the test suite.
///
/// It is the conformance fixture for the extensibility claim: registering an
/// engine must be enough, with no edit to the browser, the session controller,
/// the pairing resolver or the operation executor. It is not a product feature.
class ConformanceFlow final : public domain::IComparisonFlow {
public:
    static constexpr auto kId = "test-conformance";

    [[nodiscard]] domain::FlowDescriptor descriptor() const override {
        domain::FlowDescriptor descriptor;
        descriptor.id = QLatin1String(kId);
        descriptor.displayName = QStringLiteral("Conformance fixture");
        descriptor.stateSchemaVersion = 1;
        descriptor.minimumInputSize = 1;
        descriptor.supportedActions = QStringList{QStringLiteral("drop")};
        return descriptor;
    }

    [[nodiscard]] domain::ValidationResult
    validate(const domain::SelectionSnapshot& selection,
             const domain::FlowOptions& options) const override {
        Q_UNUSED(options)
        return selection.isEmpty() ? domain::ValidationResult::failure(QStringLiteral("empty"))
                                   : domain::ValidationResult::ok();
    }

    [[nodiscard]] domain::FlowState initialise(const domain::SelectionSnapshot& selection,
                                               const domain::FlowOptions& options) const override {
        Q_UNUSED(options)
        domain::FlowState state;
        state.flowId = QLatin1String(kId);
        state.schemaVersion = 1;
        state.revision = 1;
        state.payload.insert(QStringLiteral("remaining"),
                             domain::toJsonArray(selection.orderedAssetIds));
        state.payload.insert(QStringLiteral("rejected"), QJsonArray{});
        return state;
    }

    [[nodiscard]] domain::TransitionResult reduce(const domain::FlowState& state,
                                                  const domain::FlowAction& action) const override {
        if (action.name != QLatin1String("drop")) {
            return domain::TransitionResult::rejectedWith(QStringLiteral("unsupported"));
        }
        QList<domain::AssetId> remaining =
            domain::assetIdsFromJson(state.payload.value(QStringLiteral("remaining")).toArray());
        QList<domain::AssetId> rejected =
            domain::assetIdsFromJson(state.payload.value(QStringLiteral("rejected")).toArray());
        if (remaining.isEmpty()) {
            return domain::TransitionResult::rejectedWith(QStringLiteral("nothing left"));
        }

        const domain::AssetId dropped = remaining.takeFirst();
        rejected.append(dropped);

        domain::FlowState next = state;
        next.revision = state.revision + 1;
        next.payload.insert(QStringLiteral("remaining"), domain::toJsonArray(remaining));
        next.payload.insert(QStringLiteral("rejected"), domain::toJsonArray(rejected));

        domain::DecisionDelta delta;
        delta.rejected.append(dropped);
        return domain::TransitionResult::accept(next, delta);
    }

    [[nodiscard]] domain::FlowSummary summarise(const domain::FlowState& state) const override {
        domain::FlowSummary summary;
        summary.remaining =
            domain::assetIdsFromJson(state.payload.value(QStringLiteral("remaining")).toArray());
        summary.draftRejected =
            domain::assetIdsFromJson(state.payload.value(QStringLiteral("rejected")).toArray());
        summary.decisionsMade = static_cast<int>(summary.draftRejected.size());
        summary.complete = summary.remaining.isEmpty();
        summary.canFinish = true;
        return summary;
    }

    [[nodiscard]] domain::RestoreResult
    restore(const domain::VersionedFlowState& saved) const override {
        if (saved.flowId != QLatin1String(kId) || saved.schemaVersion != 1) {
            return domain::RestoreResult::failure(QStringLiteral("incompatible"));
        }
        domain::RestoreResult result;
        result.restored = true;
        result.state.flowId = saved.flowId;
        result.state.schemaVersion = saved.schemaVersion;
        result.state.revision = saved.revision;
        result.state.payload = saved.payload;
        return result;
    }
};

/// A flow that misbehaves in each of the ways the host must catch.
class MisbehavingFlow final : public domain::IComparisonFlow {
public:
    enum class Misbehaviour { RejectAnUnselectedPhoto, LosePhotos, DuplicatePhotos, NotAdvance };

    explicit MisbehavingFlow(Misbehaviour misbehaviour) : misbehaviour_(misbehaviour) {}

    [[nodiscard]] domain::FlowDescriptor descriptor() const override {
        domain::FlowDescriptor descriptor;
        descriptor.id = QStringLiteral("test-misbehaving");
        descriptor.displayName = QStringLiteral("Misbehaving fixture");
        descriptor.stateSchemaVersion = 1;
        descriptor.minimumInputSize = 1;
        descriptor.supportedActions = QStringList{QStringLiteral("go")};
        return descriptor;
    }

    [[nodiscard]] domain::ValidationResult validate(const domain::SelectionSnapshot&,
                                                    const domain::FlowOptions&) const override {
        return domain::ValidationResult::ok();
    }

    [[nodiscard]] domain::FlowState initialise(const domain::SelectionSnapshot& selection,
                                               const domain::FlowOptions&) const override {
        input_ = selection.orderedAssetIds;
        domain::FlowState state;
        state.flowId = descriptor().id;
        state.schemaVersion = 1;
        state.revision = 1;
        state.payload.insert(QStringLiteral("step"), 0);
        return state;
    }

    [[nodiscard]] domain::TransitionResult reduce(const domain::FlowState& state,
                                                  const domain::FlowAction&) const override {
        domain::FlowState next = state;
        next.revision =
            (misbehaviour_ == Misbehaviour::NotAdvance) ? state.revision : state.revision + 1;
        next.payload.insert(QStringLiteral("step"),
                            state.payload.value(QStringLiteral("step")).toInt() + 1);

        domain::DecisionDelta delta;
        if (misbehaviour_ == Misbehaviour::RejectAnUnselectedPhoto) {
            delta.rejected.append(domain::AssetId(QStringLiteral("not-in-the-selection")));
        }
        return domain::TransitionResult::accept(next, delta);
    }

    [[nodiscard]] domain::FlowSummary summarise(const domain::FlowState& state) const override {
        domain::FlowSummary summary;
        const int step = state.payload.value(QStringLiteral("step")).toInt();
        summary.remaining = input_;
        summary.canFinish = true;

        if (step > 0) {
            switch (misbehaviour_) {
            case Misbehaviour::RejectAnUnselectedPhoto:
                summary.draftRejected.append(
                    domain::AssetId(QStringLiteral("not-in-the-selection")));
                break;
            case Misbehaviour::LosePhotos:
                if (!summary.remaining.isEmpty()) {
                    summary.remaining.removeLast();
                }
                break;
            case Misbehaviour::DuplicatePhotos:
                if (!summary.remaining.isEmpty()) {
                    summary.remaining.append(summary.remaining.first());
                }
                break;
            case Misbehaviour::NotAdvance:
                break;
            }
        }
        return summary;
    }

    [[nodiscard]] domain::RestoreResult restore(const domain::VersionedFlowState&) const override {
        return domain::RestoreResult::failure(QStringLiteral("not supported"));
    }

private:
    Misbehaviour misbehaviour_;
    mutable QList<domain::AssetId> input_;
};

} // namespace cullfinch::testflows
