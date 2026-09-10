// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/domain/NaturalOrder.h>

namespace cullfinch::domain {
namespace {

bool isDigit(QChar character) {
    return character >= QLatin1Char('0') && character <= QLatin1Char('9');
}

/// Compare two digit runs numerically, ignoring leading zeros. Ties on value
/// fall back to the shorter (less zero-padded) run first, so ordering is total.
int compareDigitRun(const QString& lhs, qsizetype& i, const QString& rhs, qsizetype& j) {
    const qsizetype lhsStart = i;
    const qsizetype rhsStart = j;
    while (i < lhs.size() && isDigit(lhs.at(i))) {
        ++i;
    }
    while (j < rhs.size() && isDigit(rhs.at(j))) {
        ++j;
    }

    QStringView lhsRun = QStringView(lhs).sliced(lhsStart, i - lhsStart);
    QStringView rhsRun = QStringView(rhs).sliced(rhsStart, j - rhsStart);

    const qsizetype lhsPadding = lhsRun.size();
    const qsizetype rhsPadding = rhsRun.size();
    while (lhsRun.size() > 1 && lhsRun.front() == QLatin1Char('0')) {
        lhsRun = lhsRun.sliced(1);
    }
    while (rhsRun.size() > 1 && rhsRun.front() == QLatin1Char('0')) {
        rhsRun = rhsRun.sliced(1);
    }

    if (lhsRun.size() != rhsRun.size()) {
        return lhsRun.size() < rhsRun.size() ? -1 : 1;
    }
    const int digits = lhsRun.compare(rhsRun);
    if (digits != 0) {
        return digits < 0 ? -1 : 1;
    }
    if (lhsPadding != rhsPadding) {
        return lhsPadding < rhsPadding ? -1 : 1;
    }
    return 0;
}

} // namespace

int naturalCompare(const QString& lhs, const QString& rhs) {
    qsizetype i = 0;
    qsizetype j = 0;

    while (i < lhs.size() && j < rhs.size()) {
        const QChar left = lhs.at(i);
        const QChar right = rhs.at(j);

        if (isDigit(left) && isDigit(right)) {
            const int numeric = compareDigitRun(lhs, i, rhs, j);
            if (numeric != 0) {
                return numeric;
            }
            continue;
        }

        // Case-insensitive first, so `a.jpg` and `A.jpg` stay adjacent, then a
        // case-sensitive tiebreak so the ordering is total.
        const QChar leftFolded = left.toCaseFolded();
        const QChar rightFolded = right.toCaseFolded();
        if (leftFolded != rightFolded) {
            return leftFolded < rightFolded ? -1 : 1;
        }
        if (left != right) {
            return left < right ? -1 : 1;
        }
        ++i;
        ++j;
    }

    const qsizetype lhsRest = lhs.size() - i;
    const qsizetype rhsRest = rhs.size() - j;
    if (lhsRest == rhsRest) {
        return 0;
    }
    return lhsRest < rhsRest ? -1 : 1;
}

} // namespace cullfinch::domain
