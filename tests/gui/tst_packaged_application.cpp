// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/TempCollection.h>

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

using cullfinch::testsupport::TempCollection;

/// Exercises the installed application, not the build tree.
///
/// Linked unit tests cannot catch a missing runtime dependency: only launching
/// the deployed package with its own plugin layout can.
class TestPackagedApplication : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void reportsItsVersion();
    void completesASmokeRunAgainstARealJpeg();

private:
    [[nodiscard]] static QProcessEnvironment cleanEnvironment(const QString& dataDirectory,
                                                              const QString& cacheDirectory);

    QString binary_;
};

void TestPackagedApplication::initTestCase() {
    binary_ = qEnvironmentVariable("CULLFINCH_PACKAGED_BINARY");
    if (binary_.isEmpty()) {
        QSKIP("CULLFINCH_PACKAGED_BINARY is not set; run this against an installed build");
    }
    QVERIFY2(QFileInfo(binary_).isExecutable(), qPrintable(binary_));
}

QProcessEnvironment TestPackagedApplication::cleanEnvironment(const QString& dataDirectory,
                                                              const QString& cacheDirectory) {
    // A deliberately clean environment: no build-tree Qt library or plugin
    // paths may leak in, or the test would prove nothing about the package.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("QT_PLUGIN_PATH"));
    environment.remove(QStringLiteral("LD_LIBRARY_PATH"));
    environment.remove(QStringLiteral("DYLD_LIBRARY_PATH"));
    environment.remove(QStringLiteral("DYLD_FRAMEWORK_PATH"));
    environment.insert(QStringLiteral("CULLFINCH_TEST_MODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("CULLFINCH_SMOKE_DATA_DIR"), dataDirectory);
    environment.insert(QStringLiteral("CULLFINCH_SMOKE_CACHE_DIR"), cacheDirectory);
    if (!qEnvironmentVariableIsSet("DISPLAY") && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    }
    return environment;
}

void TestPackagedApplication::reportsItsVersion() {
    QTemporaryDir data;
    QTemporaryDir cache;

    QProcess process;
    process.setProcessEnvironment(cleanEnvironment(data.path(), cache.path()));
    process.start(binary_, {QStringLiteral("--version")});
    QVERIFY2(process.waitForFinished(60000), "the packaged application did not start");
    QCOMPARE(process.exitCode(), 0);
    QVERIFY(
        QString::fromUtf8(process.readAllStandardOutput()).contains(QStringLiteral("cullfinch")));
}

void TestPackagedApplication::completesASmokeRunAgainstARealJpeg() {
    TempCollection collection;
    QVERIFY(collection.isValid());
    for (int index = 1; index <= 3; ++index) {
        QVERIFY(!collection.addJpeg(QStringLiteral("IMG_%1.JPG").arg(index)).isEmpty());
        QVERIFY(!collection.addRaw(QStringLiteral("IMG_%1.RAF").arg(index)).isEmpty());
    }

    QTemporaryDir data;
    QTemporaryDir cache;

    QProcess process;
    process.setProcessEnvironment(cleanEnvironment(data.path(), cache.path()));
    process.start(binary_, {QStringLiteral("--smoke"), QStringLiteral("--data-dir"), data.path(),
                            QStringLiteral("--cache-dir"), cache.path(), collection.path()});

    QVERIFY2(process.waitForFinished(120000), "the smoke run did not finish");
    const QString output = QString::fromUtf8(process.readAllStandardOutput()) +
                           QString::fromUtf8(process.readAllStandardError());

    QVERIFY2(process.exitCode() == 0, qPrintable(output));
    // The smoke path decodes a real JPEG, opens SQLite and shows both flows, so
    // a missing image-format, platform or SQL driver plugin fails here.
    QVERIFY2(output.contains(QStringLiteral("smoke: ok")), qPrintable(output));
}

QTEST_MAIN(TestPackagedApplication)
#include "tst_packaged_application.moc"
