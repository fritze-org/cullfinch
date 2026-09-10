// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConformanceFlow.h"

#include <cullfinch/application/FlowRegistry.h>
#include <cullfinch/flows/versus/VersusFlow.h>
#include <cullfinch/flows/wall/WallFlow.h>

#include <QTest>

using cullfinch::application::FlowRegistry;
using cullfinch::testflows::ConformanceFlow;

class TestFlowRegistry : public QObject {
    Q_OBJECT

private slots:
    void registersTheShippedFlows();
    void refusesADuplicateIdentifier();
    void refusesAnInvalidDescriptor();
    void createsAFreshEngineEachTime();
    void acceptsANewFlowWithoutAnyOtherChange();
    void reportsUnknownFlowsAsNull();
};

void TestFlowRegistry::registersTheShippedFlows() {
    FlowRegistry registry;
    QVERIFY(registry.registerFlow(cullfinch::flows::versus::VersusFlow{}.descriptor(), []() {
        return std::make_unique<cullfinch::flows::versus::VersusFlow>();
    }));
    QVERIFY(registry.registerFlow(cullfinch::flows::wall::WallFlow{}.descriptor(), []() {
        return std::make_unique<cullfinch::flows::wall::WallFlow>();
    }));

    QCOMPARE(registry.descriptors().size(), 2);
    QVERIFY(registry.contains(QStringLiteral("versus-tree")));
    QVERIFY(registry.contains(QStringLiteral("image-wall")));
    QCOMPARE(registry.descriptor(QStringLiteral("image-wall")).stateSchemaVersion, 1);
}

void TestFlowRegistry::refusesADuplicateIdentifier() {
    FlowRegistry registry;
    const auto factory = []() { return std::make_unique<ConformanceFlow>(); };
    QVERIFY(registry.registerFlow(ConformanceFlow{}.descriptor(), factory));
    QVERIFY(!registry.registerFlow(ConformanceFlow{}.descriptor(), factory));
    QCOMPARE(registry.descriptors().size(), 1);
}

void TestFlowRegistry::refusesAnInvalidDescriptor() {
    FlowRegistry registry;
    cullfinch::domain::FlowDescriptor descriptor;
    QVERIFY(
        !registry.registerFlow(descriptor, []() { return std::make_unique<ConformanceFlow>(); }));
    QVERIFY(!registry.registerFlow(ConformanceFlow{}.descriptor(), nullptr));
}

void TestFlowRegistry::createsAFreshEngineEachTime() {
    FlowRegistry registry;
    registry.registerFlow(ConformanceFlow{}.descriptor(),
                          []() { return std::make_unique<ConformanceFlow>(); });

    auto first = registry.create(QLatin1String(ConformanceFlow::kId));
    auto second = registry.create(QLatin1String(ConformanceFlow::kId));
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);
    QVERIFY(first.get() != second.get());
}

void TestFlowRegistry::acceptsANewFlowWithoutAnyOtherChange() {
    // The milestone-3 exit criterion: an arbitrary new flow registers and is
    // discoverable, with no edit anywhere else in the application.
    FlowRegistry registry;
    QVERIFY(registry.registerFlow(ConformanceFlow{}.descriptor(),
                                  []() { return std::make_unique<ConformanceFlow>(); }));

    bool found = false;
    for (const cullfinch::domain::FlowDescriptor& descriptor : registry.descriptors()) {
        if (descriptor.id == QLatin1String(ConformanceFlow::kId)) {
            found = true;
            QCOMPARE(descriptor.displayName, QStringLiteral("Conformance fixture"));
        }
    }
    QVERIFY(found);
}

void TestFlowRegistry::reportsUnknownFlowsAsNull() {
    const FlowRegistry registry;
    QVERIFY(!registry.contains(QStringLiteral("nope")));
    QVERIFY(registry.create(QStringLiteral("nope")) == nullptr);
    QVERIFY(!registry.descriptor(QStringLiteral("nope")).isValid());
}

QTEST_APPLESS_MAIN(TestFlowRegistry)
#include "tst_flow_registry.moc"
