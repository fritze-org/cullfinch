// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/DirectoryScanner.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <QFile>
#include <QSignalSpy>
#include <QTest>

#include <tuple>

using namespace cullfinch;
using cullfinch::testsupport::TempCollection;

class TestDirectoryScanner : public QObject {
    Q_OBJECT

private slots:
    void enumeratesJpegAndRawFiles();
    void reportsAnUnreadableRootAsAnError();
    void skipsTheStagingDirectory();
    void recursesOnlyWhenAsked();
    void preservesRawBytesExactly();
    void handlesUnicodeFilenames();
    void producesOneAssetPerJpegRawPair();
    void discardsResultsFromASupersededGeneration();
    void reportsCaseDistinctStemsWithoutMerging();
};

void TestDirectoryScanner::enumeratesJpegAndRawFiles() {
    TempCollection collection;
    QVERIFY(collection.isValid());
    QVERIFY(!collection.addJpeg(QStringLiteral("DSCF0001.JPG")).isEmpty());
    QVERIFY(!collection.addRaw(QStringLiteral("DSCF0001.RAF")).isEmpty());

    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error);

    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(files.size(), 2);
    for (const domain::DiscoveredFile& file : files) {
        QCOMPARE(file.stem, QStringLiteral("DSCF0001"));
        QVERIFY(file.relativeDirectory.isEmpty());
        QVERIFY(file.fingerprint.isKnown());
        QVERIFY(file.fingerprint.sizeBytes > 0);
    }
}

void TestDirectoryScanner::reportsAnUnreadableRootAsAnError() {
    QString error;
    const QList<domain::DiscoveredFile> files = infrastructure::DirectoryScanner::enumerate(
        QStringLiteral("/definitely/not/a/directory"), false, &error);
    QVERIFY(files.isEmpty());
    QVERIFY(!error.isEmpty());
}

void TestDirectoryScanner::skipsTheStagingDirectory() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addJpeg(QStringLiteral(".cullfinch-staging/group/B.JPG"));

    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection.path(), true, &error);

    // Staged files are mid-operation, not part of the collection.
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().stem, QStringLiteral("A"));
}

void TestDirectoryScanner::recursesOnlyWhenAsked() {
    TempCollection collection;
    collection.addJpeg(QStringLiteral("top.JPG"));
    collection.addJpeg(QStringLiteral("day1/nested.JPG"));

    QString error;
    QCOMPARE(infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error).size(),
             1);

    const QList<domain::DiscoveredFile> recursive =
        infrastructure::DirectoryScanner::enumerate(collection.path(), true, &error);
    QCOMPARE(recursive.size(), 2);

    bool sawRelativeDirectory = false;
    for (const domain::DiscoveredFile& file : recursive) {
        if (file.stem == QStringLiteral("nested")) {
            // Recursive mode preserves the relative directory in grouping keys.
            QCOMPARE(file.relativeDirectory, QStringLiteral("day1"));
            sawRelativeDirectory = true;
        }
    }
    QVERIFY(sawRelativeDirectory);
}

void TestDirectoryScanner::preservesRawBytesExactly() {
    TempCollection collection;
    const QByteArray payload = QByteArrayLiteral("\x00\x01\xfe\xffopaque-raw-bytes");
    collection.addJpeg(QStringLiteral("A.JPG"));
    const QString rawPath = collection.addRaw(QStringLiteral("A.RAF"), payload);

    QString error;
    std::ignore = infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error);

    // cullfinch never decodes RAW bytes, and browsing opens files read-only.
    QFile file(rawPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), payload);
}

void TestDirectoryScanner::handlesUnicodeFilenames() {
    TempCollection collection;
    const QString name = QString::fromUtf8("Ünïcødé_Ωmega.JPG");
    QVERIFY(!collection.addJpeg(name).isEmpty());

    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error);
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().fileName, name);
}

void TestDirectoryScanner::producesOneAssetPerJpegRawPair() {
    TempCollection collection;
    for (int index = 1; index <= 3; ++index) {
        collection.addJpeg(QStringLiteral("IMG_%1.JPG").arg(index));
        collection.addRaw(QStringLiteral("IMG_%1.CR3").arg(index));
    }

    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error);

    const domain::StemAssociationResolver resolver;
    const domain::AssociationResult result =
        resolver.resolve(domain::CollectionId(QStringLiteral("c1")), files,
                         domain::AssociationConfig::defaults(), true);

    QCOMPARE(result.assets.size(), 3);
    for (const domain::PhotoAsset& asset : result.assets) {
        QCOMPARE(asset.pairingState, domain::PairingState::Resolved);
        QCOMPARE(asset.members.size(), 2);
        QVERIFY(asset.isOperable());
        // The native identity was read, so a replaced file will be noticed.
        QVERIFY(asset.members.first().fingerprint.native.known);
    }
}

void TestDirectoryScanner::discardsResultsFromASupersededGeneration() {
    TempCollection first;
    first.addJpeg(QStringLiteral("A.JPG"));
    TempCollection second;
    second.addJpeg(QStringLiteral("B.JPG"));

    infrastructure::DirectoryScanner scanner;
    QSignalSpy finished(&scanner, &application::IScanService::scanFinished);

    application::ScanRequest stale;
    stale.collectionId = domain::CollectionId(QStringLiteral("c1"));
    stale.rootPath = first.path();
    stale.config = domain::AssociationConfig::defaults();
    stale.generation = 1;
    scanner.requestScan(stale);

    application::ScanRequest current = stale;
    current.rootPath = second.path();
    current.generation = 2;
    scanner.requestScan(current);

    QVERIFY(finished.wait(15000));
    // Whatever arrives must belong to the current generation only.
    for (const QList<QVariant>& emission : finished) {
        QCOMPARE(emission.at(0).toULongLong(), 2ULL);
    }
}

void TestDirectoryScanner::reportsCaseDistinctStemsWithoutMerging() {
    TempCollection collection;
    if (!collection.supportsCaseDistinctNames()) {
        // A case-insensitive volume cannot hold both names; that is a
        // legitimate skip, not a failure.
        QSKIP("this filesystem folds filename case");
    }

    collection.addJpeg(QStringLiteral("A.JPG"));
    collection.addRaw(QStringLiteral("a.RAF"));

    QString error;
    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collection.path(), false, &error);

    const domain::StemAssociationResolver resolver;
    const domain::AssociationResult result =
        resolver.resolve(domain::CollectionId(QStringLiteral("c1")), files,
                         domain::AssociationConfig::defaults(), true);

    QCOMPARE(result.assets.size(), 2);
    for (const domain::PhotoAsset& asset : result.assets) {
        QCOMPARE(asset.pairingState, domain::PairingState::Ambiguous);
        QVERIFY(!asset.isOperable());
    }
}

QTEST_MAIN(TestDirectoryScanner)
#include "tst_directory_scanner.moc"
