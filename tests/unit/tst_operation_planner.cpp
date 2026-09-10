// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/OperationPlan.h>
#include <cullfinch/testsupport/AssetBuilder.h>

#include <QSet>
#include <QTest>

using namespace cullfinch::domain;
using cullfinch::testsupport::AssetBuilder;

namespace {

PlanningResult planFor(const PhotoAssetList& assets) {
    return OperationPlanner::plan(CollectionId(QStringLiteral("c1")), 3,
                                  QStringLiteral("/photos/.cullfinch-staging"), assets);
}

} // namespace

class TestOperationPlanner : public QObject {
    Q_OBJECT

private slots:
    void plansTheWholeGroupIncludingEveryRaw();
    void countsPhotosFilesAndBytes();
    void refusesAnAssetThatIsNotMarked();
    void refusesAmbiguousStaleAndProvisionalGroups_data();
    void refusesAmbiguousStaleAndProvisionalGroups();
    void refusesAGroupWhoseOperationsAreBlocked();
    void neverPlacesAFileInTwoGroups();
    void givesEachGroupItsOwnStagingDirectory();
};

void TestOperationPlanner::plansTheWholeGroupIncludingEveryRaw() {
    const PhotoAsset asset = AssetBuilder(QStringLiteral("A"))
                                 .withJpeg()
                                 .withRaw(QStringLiteral("cr3"))
                                 .withRaw(QStringLiteral("dng"))
                                 .withDisposition(Disposition::Reject)
                                 .build();

    const PlanningResult result = planFor({asset});
    QVERIFY(!result.hasBlockers());
    QCOMPARE(result.plan.groups.size(), 1);

    // An operation is planned for an entire asset, never for a lone JPG.
    const PlannedGroup& group = result.plan.groups.first();
    QCOMPARE(group.members.size(), 3);

    QSet<QString> roles;
    for (const PlannedMember& member : group.members) {
        roles.insert(memberRoleName(member.role));
    }
    QVERIFY(roles.contains(memberRoleName(MemberRole::Jpeg)));
    QVERIFY(roles.contains(memberRoleName(MemberRole::Raw)));
}

void TestOperationPlanner::countsPhotosFilesAndBytes() {
    PhotoAssetList assets;
    for (int index = 0; index < 3; ++index) {
        assets.append(AssetBuilder(QStringLiteral("IMG_%1").arg(index))
                          .withJpeg(1000)
                          .withRaw(QStringLiteral("raf"), 9000)
                          .withDisposition(Disposition::Reject)
                          .build());
    }

    const PlanningResult result = planFor(assets);
    QCOMPARE(result.plan.logicalPhotoCount(), 3);
    QCOMPARE(result.plan.physicalFileCount(), 6);
    QCOMPARE(result.plan.totalBytes(), 3 * (1000 + 9000));
}

void TestOperationPlanner::refusesAnAssetThatIsNotMarked() {
    const PhotoAsset neutral = AssetBuilder(QStringLiteral("A")).withJpeg().withRaw().build();

    const PlanningResult result = planFor({neutral});
    QVERIFY(result.plan.isEmpty());
    QCOMPARE(result.blocked.size(), 1);
}

void TestOperationPlanner::refusesAmbiguousStaleAndProvisionalGroups_data() {
    QTest::addColumn<int>("pairing");
    QTest::newRow("ambiguous") << static_cast<int>(PairingState::Ambiguous);
    QTest::newRow("stale") << static_cast<int>(PairingState::Stale);
    QTest::newRow("provisional") << static_cast<int>(PairingState::Provisional);
}

void TestOperationPlanner::refusesAmbiguousStaleAndProvisionalGroups() {
    QFETCH(int, pairing);
    const PhotoAsset asset = AssetBuilder(QStringLiteral("A"))
                                 .withJpeg()
                                 .withRaw()
                                 .withPairing(static_cast<PairingState>(pairing))
                                 .withDisposition(Disposition::Reject)
                                 .build();

    const PlanningResult result = planFor({asset});
    QVERIFY2(result.plan.isEmpty(), "an unresolved group must never reach a physical operation");
    QCOMPARE(result.blocked.size(), 1);
    QVERIFY(!result.blocked.first().reason.isEmpty());
}

void TestOperationPlanner::refusesAGroupWhoseOperationsAreBlocked() {
    const PhotoAsset asset = AssetBuilder(QStringLiteral("A"))
                                 .withJpeg()
                                 .withRaw()
                                 .withDisposition(Disposition::Reject)
                                 .withOperationsBlocked(true)
                                 .build();

    const PlanningResult result = planFor({asset});
    QVERIFY(result.plan.isEmpty());
    QCOMPARE(result.blocked.size(), 1);
}

void TestOperationPlanner::neverPlacesAFileInTwoGroups() {
    // Two assets that both claim the same physical file: an association
    // conflict that must block both rather than deduplicate silently.
    PhotoAsset first =
        AssetBuilder(QStringLiteral("A")).withJpeg().withDisposition(Disposition::Reject).build();
    PhotoAsset second =
        AssetBuilder(QStringLiteral("B")).withJpeg().withDisposition(Disposition::Reject).build();
    second.members[0].absolutePath = first.members.at(0).absolutePath;

    const PlanningResult result = planFor({first, second});
    QVERIFY(result.plan.isEmpty());
    QCOMPARE(result.blocked.size(), 2);
}

void TestOperationPlanner::givesEachGroupItsOwnStagingDirectory() {
    PhotoAssetList assets;
    for (int index = 0; index < 4; ++index) {
        assets.append(AssetBuilder(QStringLiteral("IMG_%1").arg(index))
                          .withJpeg()
                          .withRaw()
                          .withDisposition(Disposition::Reject)
                          .build());
    }

    const PlanningResult result = planFor(assets);
    QSet<QString> directories;
    QSet<QString> paths;
    for (const PlannedGroup& group : result.plan.groups) {
        QVERIFY(!group.stagingDirectoryName.isEmpty());
        directories.insert(group.stagingDirectoryName);
        for (const PlannedMember& member : group.members) {
            QVERIFY2(!paths.contains(member.sourcePath), "a file appeared in two groups");
            paths.insert(member.sourcePath);
        }
    }
    QCOMPARE(directories.size(), result.plan.groups.size());
}

QTEST_APPLESS_MAIN(TestOperationPlanner)
#include "tst_operation_planner.moc"
