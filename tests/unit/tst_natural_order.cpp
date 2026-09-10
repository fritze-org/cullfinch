// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/NaturalOrder.h>

#include <QTest>

#include <algorithm>

using cullfinch::domain::naturalCompare;
using cullfinch::domain::naturalLess;

class TestNaturalOrder : public QObject {
    Q_OBJECT

private slots:
    void comparesDigitRunsNumerically_data();
    void comparesDigitRunsNumerically();
    void isATotalOrder();
    void sortsAFilenameSeriesInCaptureOrder();
};

void TestNaturalOrder::comparesDigitRunsNumerically_data() {
    QTest::addColumn<QString>("left");
    QTest::addColumn<QString>("right");
    QTest::addColumn<int>("sign");

    QTest::newRow("nine before ten") << QStringLiteral("IMG_9") << QStringLiteral("IMG_10") << -1;
    QTest::newRow("padding ignored")
        << QStringLiteral("IMG_0009") << QStringLiteral("IMG_10") << -1;
    QTest::newRow("equal") << QStringLiteral("IMG_10") << QStringLiteral("IMG_10") << 0;
    QTest::newRow("case insensitive first")
        << QStringLiteral("a.jpg") << QStringLiteral("B.jpg") << -1;
    QTest::newRow("case tiebreak is stable") << QStringLiteral("A") << QStringLiteral("a") << -1;
    QTest::newRow("prefix is shorter") << QStringLiteral("IMG") << QStringLiteral("IMG_1") << -1;
}

void TestNaturalOrder::comparesDigitRunsNumerically() {
    QFETCH(QString, left);
    QFETCH(QString, right);
    QFETCH(int, sign);

    const int result = naturalCompare(left, right);
    QCOMPARE(result < 0, sign < 0);
    QCOMPARE(result > 0, sign > 0);
    QCOMPARE(result == 0, sign == 0);

    // Antisymmetry: the reverse comparison must have the opposite sign.
    const int reversed = naturalCompare(right, left);
    QCOMPARE(reversed < 0, sign > 0);
    QCOMPARE(reversed > 0, sign < 0);
}

void TestNaturalOrder::isATotalOrder() {
    const QStringList names{QStringLiteral("IMG_2"),    QStringLiteral("IMG_10"),
                            QStringLiteral("img_2"),    QStringLiteral("IMG_02"),
                            QStringLiteral("DSCF0001"), QStringLiteral("DSCF1")};

    // Every pair is ordered consistently in both directions.
    for (const QString& left : names) {
        for (const QString& right : names) {
            const int forward = naturalCompare(left, right);
            const int backward = naturalCompare(right, left);
            QCOMPARE(forward == 0, backward == 0);
            QCOMPARE(forward < 0, backward > 0);
        }
    }
}

void TestNaturalOrder::sortsAFilenameSeriesInCaptureOrder() {
    QStringList names{QStringLiteral("IMG_100"), QStringLiteral("IMG_9"), QStringLiteral("IMG_10"),
                      QStringLiteral("IMG_1")};
    std::sort(names.begin(), names.end(), naturalLess);

    QCOMPARE(names, (QStringList{QStringLiteral("IMG_1"), QStringLiteral("IMG_9"),
                                 QStringLiteral("IMG_10"), QStringLiteral("IMG_100")}));
}

QTEST_APPLESS_MAIN(TestNaturalOrder)
#include "tst_natural_order.moc"
