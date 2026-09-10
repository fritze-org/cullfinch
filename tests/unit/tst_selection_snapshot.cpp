// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/SelectionSnapshot.h>
#include <cullfinch/testsupport/AssetBuilder.h>

#include <QTest>

using namespace cullfinch::domain;
using cullfinch::testsupport::AssetBuilder;

class TestSelectionSnapshot : public QObject {
    Q_OBJECT

private slots:
    void freezesOrderAndMembershipRevisions();
    void excludesEachIneligibleReasonSeparately_data();
    void excludesEachIneligibleReasonSeparately();
    void reportsWhatWasLeftOut();
    void ignoresDuplicateSelections();
};

void TestSelectionSnapshot::freezesOrderAndMembershipRevisions() {
    PhotoAssetList assets = AssetBuilder::resolvedSeries(4);
    assets[2].membershipRevision = 99;

    const SelectionSnapshot snapshot =
        SelectionSnapshot::freeze(CollectionId(QStringLiteral("c1")), 7, assets);

    QCOMPARE(snapshot.size(), 4);
    QCOMPARE(snapshot.collectionRevision, 7U);
    QCOMPARE(snapshot.orderedAssetIds.first(), assets.first().id);
    QCOMPARE(snapshot.membershipRevisions.value(assets.at(2).id), 99U);
    QCOMPARE(snapshot.indexOf(assets.at(2).id), 2);
    QVERIFY(snapshot.contains(assets.at(3).id));
}

void TestSelectionSnapshot::excludesEachIneligibleReasonSeparately_data() {
    QTest::addColumn<int>("pairing");
    QTest::addColumn<int>("disposition");
    QTest::addColumn<bool>("eligible");

    QTest::newRow("resolved") << static_cast<int>(PairingState::Resolved)
                              << static_cast<int>(Disposition::Neutral) << true;
    QTest::newRow("jpeg only") << static_cast<int>(PairingState::JpegOnly)
                               << static_cast<int>(Disposition::Neutral) << true;
    QTest::newRow("already rejected") << static_cast<int>(PairingState::Resolved)
                                      << static_cast<int>(Disposition::Reject) << false;
    QTest::newRow("provisional") << static_cast<int>(PairingState::Provisional)
                                 << static_cast<int>(Disposition::Neutral) << false;
    QTest::newRow("raw only") << static_cast<int>(PairingState::RawOnly)
                              << static_cast<int>(Disposition::Neutral) << false;
    QTest::newRow("ambiguous") << static_cast<int>(PairingState::Ambiguous)
                               << static_cast<int>(Disposition::Neutral) << false;
    QTest::newRow("stale") << static_cast<int>(PairingState::Stale)
                           << static_cast<int>(Disposition::Neutral) << false;
}

void TestSelectionSnapshot::excludesEachIneligibleReasonSeparately() {
    QFETCH(int, pairing);
    QFETCH(int, disposition);
    QFETCH(bool, eligible);

    const PhotoAsset asset = AssetBuilder(QStringLiteral("A"))
                                 .withJpeg()
                                 .withRaw()
                                 .withPairing(static_cast<PairingState>(pairing))
                                 .withDisposition(static_cast<Disposition>(disposition))
                                 .build();

    const SelectionEligibility eligibility =
        selectEligible(CollectionId(QStringLiteral("c1")), 1, {asset});

    QCOMPARE(eligibility.snapshot.size(), eligible ? 1 : 0);
    QCOMPARE(eligibility.hasExclusions(), !eligible);
    if (!eligible) {
        // Every exclusion carries an explanation the browser can show.
        QVERIFY(!eligibility.excluded.first().reason.isEmpty());
        QCOMPARE(eligibility.excluded.first().id, asset.id);
    }
}

void TestSelectionSnapshot::reportsWhatWasLeftOut() {
    PhotoAssetList assets = AssetBuilder::resolvedSeries(3);
    assets[1].disposition = Disposition::Reject;
    assets[2].pairingState = PairingState::Ambiguous;

    const SelectionEligibility eligibility =
        selectEligible(CollectionId(QStringLiteral("c1")), 1, assets);

    QCOMPARE(eligibility.snapshot.size(), 1);
    QCOMPARE(eligibility.excluded.size(), 2);
    // An already-rejected photo is excluded until the user unmarks it.
    QCOMPARE(eligibility.excluded.first().id, assets.at(1).id);
}

void TestSelectionSnapshot::ignoresDuplicateSelections() {
    const PhotoAssetList assets = AssetBuilder::resolvedSeries(2);
    const PhotoAssetList duplicated{assets.at(0), assets.at(1), assets.at(0)};

    const SelectionSnapshot snapshot =
        SelectionSnapshot::freeze(CollectionId(QStringLiteral("c1")), 1, duplicated);
    QCOMPARE(snapshot.size(), 2);
}

QTEST_APPLESS_MAIN(TestSelectionSnapshot)
#include "tst_selection_snapshot.moc"
