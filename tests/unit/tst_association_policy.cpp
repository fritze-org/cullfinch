// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/AssociationPolicy.h>

#include <QTest>

using namespace cullfinch::domain;

namespace {

DiscoveredFile file(const QString& name, const QString& directory = QString()) {
    DiscoveredFile discovered;
    discovered.fileName = name;
    discovered.relativeDirectory = directory;
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    discovered.stem = dot > 0 ? name.left(dot) : name;
    discovered.extension = dot > 0 ? name.mid(dot + 1) : QString();
    discovered.absolutePath = QStringLiteral("/photos/") +
                              (directory.isEmpty() ? QString() : directory + QLatin1Char('/')) +
                              name;
    discovered.fingerprint.sizeBytes = 1024;
    discovered.fingerprint.modifiedMsecsUtc = 1'700'000'000'000;
    return discovered;
}

AssociationResult resolve(const QList<DiscoveredFile>& files, bool scopeComplete = true) {
    const StemAssociationResolver resolver;
    return resolver.resolve(CollectionId(QStringLiteral("c1")), files,
                            AssociationConfig::defaults(), scopeComplete);
}

const PhotoAsset* findByStem(const AssociationResult& result, const QString& stem) {
    for (const PhotoAsset& asset : result.assets) {
        if (asset.stem == stem) {
            return &asset;
        }
    }
    return nullptr;
}

} // namespace

/// The table in section 4.2 of the design, executed.
class TestAssociationPolicy : public QObject {
    Q_OBJECT

private slots:
    void confirmedPairingTable_data();
    void confirmedPairingTable();
    void showsTheJpegAsThePreview();
    void keepsSeveralRawCompanionsInOneGroup();
    void neverPairsAcrossDirectories();
    void flagsStemCaseMismatchInsteadOfDiscardingTheCompanion();
    void recordsUnclassifiedSameStemFilesWithoutBlockingComparison();
    void ignoresSidecarsWhileSidecarHandlingIsOff();
    void flagsSymlinksForOperationsOnly();
    void flagsHardlinkAliases();
    void leavesEverythingProvisionalUntilTheScopeIsComplete();
    void producesDeterministicIdentitiesAndOrder();
};

void TestAssociationPolicy::confirmedPairingTable_data() {
    QTest::addColumn<QStringList>("names");
    QTest::addColumn<QString>("stem");
    QTest::addColumn<int>("pairing");
    QTest::addColumn<int>("memberCount");

    QTest::newRow("JPG + RAF") << QStringList{QStringLiteral("DSCF0123.JPG"),
                                              QStringLiteral("DSCF0123.RAF")}
                               << QStringLiteral("DSCF0123")
                               << static_cast<int>(PairingState::Resolved) << 2;
    QTest::newRow("jpeg + CR3") << QStringList{QStringLiteral("IMG_0123.jpeg"),
                                               QStringLiteral("IMG_0123.CR3")}
                                << QStringLiteral("IMG_0123")
                                << static_cast<int>(PairingState::Resolved) << 2;
    QTest::newRow("two RAW companions")
        << QStringList{QStringLiteral("A.JPG"), QStringLiteral("A.CR3"), QStringLiteral("A.DNG")}
        << QStringLiteral("A") << static_cast<int>(PairingState::Resolved) << 3;
    QTest::newRow("JPG only") << QStringList{QStringLiteral("A.JPG")} << QStringLiteral("A")
                              << static_cast<int>(PairingState::JpegOnly) << 1;
    QTest::newRow("RAW only") << QStringList{QStringLiteral("A.RAF")} << QStringLiteral("A")
                              << static_cast<int>(PairingState::RawOnly) << 1;
    QTest::newRow("ambiguous preview")
        << QStringList{QStringLiteral("A.JPG"), QStringLiteral("A.jpeg"), QStringLiteral("A.RAF")}
        << QStringLiteral("A") << static_cast<int>(PairingState::Ambiguous) << 3;
}

void TestAssociationPolicy::confirmedPairingTable() {
    QFETCH(QStringList, names);
    QFETCH(QString, stem);
    QFETCH(int, pairing);
    QFETCH(int, memberCount);

    QList<DiscoveredFile> files;
    for (const QString& name : names) {
        files.append(file(name));
    }

    const AssociationResult result = resolve(files);
    const PhotoAsset* asset = findByStem(result, stem);
    QVERIFY(asset != nullptr);
    QCOMPARE(static_cast<int>(asset->pairingState), pairing);
    QCOMPARE(static_cast<int>(asset->members.size()), memberCount);
}

void TestAssociationPolicy::showsTheJpegAsThePreview() {
    const AssociationResult result =
        resolve({file(QStringLiteral("DSCF0123.RAF")), file(QStringLiteral("DSCF0123.JPG"))});
    const PhotoAsset* asset = findByStem(result, QStringLiteral("DSCF0123"));
    QVERIFY(asset != nullptr);

    const FileMember* preview = asset->preview();
    QVERIFY(preview != nullptr);
    QCOMPARE(preview->role, MemberRole::Jpeg);
    QVERIFY(asset->isComparable());
    QVERIFY(asset->isOperable());
}

void TestAssociationPolicy::keepsSeveralRawCompanionsInOneGroup() {
    const AssociationResult result =
        resolve({file(QStringLiteral("A.JPG")), file(QStringLiteral("A.CR3")),
                 file(QStringLiteral("A.DNG"))});
    QCOMPARE(result.assets.size(), 1);
    QCOMPARE(result.assets.first().rawCount(), 2);
}

void TestAssociationPolicy::neverPairsAcrossDirectories() {
    const AssociationResult result =
        resolve({file(QStringLiteral("A.JPG"), QStringLiteral("day1")),
                 file(QStringLiteral("A.RAF"), QStringLiteral("day2"))});

    QCOMPARE(result.assets.size(), 2);
    for (const PhotoAsset& asset : result.assets) {
        QCOMPARE(asset.members.size(), 1);
    }
}

void TestAssociationPolicy::flagsStemCaseMismatchInsteadOfDiscardingTheCompanion() {
    const AssociationResult result =
        resolve({file(QStringLiteral("A.JPG")), file(QStringLiteral("a.RAF"))});

    // Two separate assets, both flagged: the possible companion is never
    // silently discarded and the groups are never silently merged.
    QCOMPARE(result.assets.size(), 2);
    for (const PhotoAsset& asset : result.assets) {
        QCOMPARE(asset.pairingState, PairingState::Ambiguous);
        QVERIFY(asset.operationsBlocked);
        QVERIFY(!asset.diagnostics.isEmpty());
    }
    QVERIFY(!result.diagnostics.isEmpty());
}

void TestAssociationPolicy::recordsUnclassifiedSameStemFilesWithoutBlockingComparison() {
    const AssociationResult result =
        resolve({file(QStringLiteral("A.JPG")), file(QStringLiteral("A.RAF")),
                 file(QStringLiteral("A.txt"))});

    QCOMPARE(result.assets.size(), 1);
    const PhotoAsset& asset = result.assets.first();
    QCOMPARE(asset.pairingState, PairingState::Resolved);
    // Comparison is unaffected; only file operations are held back.
    QVERIFY(asset.isComparable());
    QVERIFY(!asset.isOperable());
    QVERIFY(asset.operationsBlocked);
}

void TestAssociationPolicy::ignoresSidecarsWhileSidecarHandlingIsOff() {
    const AssociationResult result =
        resolve({file(QStringLiteral("A.JPG")), file(QStringLiteral("A.xmp"))});

    QCOMPARE(result.assets.size(), 1);
    const PhotoAsset& asset = result.assets.first();
    QCOMPARE(asset.members.size(), 1);
    QVERIFY(asset.isOperable());

    // With sidecars enabled the same file joins the operation group.
    AssociationConfig config = AssociationConfig::defaults();
    config.sidecarsEnabled = true;
    const StemAssociationResolver resolver;
    const AssociationResult enabled = resolver.resolve(
        CollectionId(QStringLiteral("c1")),
        {file(QStringLiteral("A.JPG")), file(QStringLiteral("A.xmp"))}, config, true);
    QCOMPARE(enabled.assets.first().members.size(), 2);
    QVERIFY(enabled.assets.first().isOperable());
}

void TestAssociationPolicy::flagsSymlinksForOperationsOnly() {
    DiscoveredFile link = file(QStringLiteral("A.JPG"));
    link.isSymlink = true;
    const AssociationResult result = resolve({link});

    const PhotoAsset& asset = result.assets.first();
    QVERIFY(asset.isComparable());
    QVERIFY(!asset.isOperable());
}

void TestAssociationPolicy::flagsHardlinkAliases() {
    DiscoveredFile first = file(QStringLiteral("A.JPG"));
    first.fingerprint.native = NativeIdentity{7, 42, true};
    DiscoveredFile second = file(QStringLiteral("B.JPG"));
    second.fingerprint.native = NativeIdentity{7, 42, true};

    const AssociationResult result = resolve({first, second});
    QCOMPARE(result.assets.size(), 2);
    for (const PhotoAsset& asset : result.assets) {
        QVERIFY2(!asset.isOperable(), "a hardlink alias must not be operated on twice");
    }
    QVERIFY(!result.diagnostics.isEmpty());
}

void TestAssociationPolicy::leavesEverythingProvisionalUntilTheScopeIsComplete() {
    const AssociationResult partial =
        resolve({file(QStringLiteral("A.JPG"))}, /*scopeComplete=*/false);

    const PhotoAsset& asset = partial.assets.first();
    QCOMPARE(asset.pairingState, PairingState::Provisional);
    // A provisional group can neither be compared nor operated on: a JPG seen
    // before its RAW would otherwise acquire incorrect membership.
    QVERIFY(!asset.isComparable());
    QVERIFY(!asset.isOperable());
}

void TestAssociationPolicy::producesDeterministicIdentitiesAndOrder() {
    const QList<DiscoveredFile> shuffled{file(QStringLiteral("IMG_10.JPG")),
                                         file(QStringLiteral("IMG_2.JPG")),
                                         file(QStringLiteral("IMG_1.JPG"))};
    const AssociationResult first = resolve(shuffled);
    const AssociationResult second = resolve(shuffled);

    QCOMPARE(first.assets.size(), 3);
    // Natural filename ordering, independent of discovery order.
    QCOMPARE(first.assets.at(0).stem, QStringLiteral("IMG_1"));
    QCOMPARE(first.assets.at(1).stem, QStringLiteral("IMG_2"));
    QCOMPARE(first.assets.at(2).stem, QStringLiteral("IMG_10"));

    for (int index = 0; index < first.assets.size(); ++index) {
        QCOMPARE(first.assets.at(index).id.toString(), second.assets.at(index).id.toString());
        QCOMPARE(first.assets.at(index).membershipRevision,
                 second.assets.at(index).membershipRevision);
    }
}

QTEST_APPLESS_MAIN(TestAssociationPolicy)
#include "tst_association_policy.moc"
