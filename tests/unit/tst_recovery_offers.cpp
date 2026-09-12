// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/Services.h>

#include <QStringList>
#include <QTest>

using namespace cullfinch;

namespace {

/// A record with one group per entry in `steps`, every member of group i left
/// at steps[i].
///
/// Two members per group on purpose: a group is the unit an offer applies to,
/// and a single-file group cannot show what happens when its files disagree.
application::OperationRecord recordWith(const QStringList& steps, domain::OperationState state) {
    application::OperationRecord record;
    record.state = state;
    for (int groupIndex = 0; groupIndex < steps.size(); ++groupIndex) {
        domain::PlannedGroup group;
        group.assetId = domain::AssetId(QStringLiteral("asset%1").arg(groupIndex));
        group.displayName = QStringLiteral("IMG_%1").arg(groupIndex);

        for (int memberIndex = 0; memberIndex < 2; ++memberIndex) {
            domain::PlannedMember planned;
            planned.memberId =
                domain::MemberId(QStringLiteral("m%1-%2").arg(groupIndex).arg(memberIndex));
            planned.fileName =
                QStringLiteral("IMG_%1.%2")
                    .arg(groupIndex)
                    .arg(memberIndex == 0 ? QStringLiteral("JPG") : QStringLiteral("RAF"));
            group.members.append(planned);

            application::OperationMemberRecord entry;
            entry.memberId = planned.memberId;
            entry.assetId = group.assetId;
            entry.lastDurableStep = steps.at(groupIndex);
            record.members.append(entry);
        }
        record.plan.groups.append(group);
    }
    return record;
}

QString stepOf(QLatin1String step) {
    return {step};
}

} // namespace

/// What the recovery screen may offer about a stopped operation, decided from
/// the journal alone.
///
/// This is the whole of the decision: the executor revalidates against the
/// filesystem when an offer is taken up, but what a person is shown -- and so
/// what they can ask for -- comes from here and nothing else. It is worth
/// pinning down without a filesystem in the way.
class TestRecoveryOffers : public QObject {
    Q_OBJECT

private slots:
    void offersFollowWhereEachGroupIs_data();
    void offersFollowWhereEachGroupIs();
    void aGroupWhoseFilesDisagreeIsCountedAsNothing();
    void aGroupTheJournalDoesNotKnowIsCountedAsNothing();
    void anOperationWithNoGroupsOffersNothing();
    void aCompletedOperationOffersNothing();
    void onlyWorkLeftOnDiskNeedsAttention_data();
    void onlyWorkLeftOnDiskNeedsAttention();
};

void TestRecoveryOffers::offersFollowWhereEachGroupIs_data() {
    QTest::addColumn<QStringList>("steps");
    QTest::addColumn<bool>("restore");
    QTest::addColumn<bool>("retryTrash");
    QTest::addColumn<bool>("confirmTrashed");

    namespace step = application::operationStep;

    // Nothing has moved, so there is nothing to put back or hand over.
    QTest::newRow("all at their original paths")
        << QStringList{stepOf(step::planned)} << false << false << false;
    QTest::newRow("all put back") << QStringList{stepOf(step::restored)} << false << false << false;
    QTest::newRow("all in Trash") << QStringList{stepOf(step::trashed)} << false << false << false;

    // A complete group in staging can go back, or on to Trash.
    QTest::newRow("the only group staged")
        << QStringList{stepOf(step::staged)} << true << true << false;
    QTest::newRow("staged beside a group already trashed")
        << QStringList{stepOf(step::staged), stepOf(step::trashed)} << true << true << false;

    // One group never left its original paths, so the plan no longer describes
    // the work: restore and review again, never trash what is staged.
    QTest::newRow("staged beside a group never moved")
        << QStringList{stepOf(step::staged), stepOf(step::planned)} << true << false << false;

    // Only a person can settle an outcome the filesystem cannot show.
    QTest::newRow("the only group uncertain")
        << QStringList{stepOf(step::uncertain)} << false << false << true;
    QTest::newRow("uncertain beside a group already trashed")
        << QStringList{stepOf(step::uncertain), stepOf(step::trashed)} << false << false << true;

    // ...but not while something else about the operation is still in the air:
    // confirming would be answering for a group nobody looked at.
    QTest::newRow("uncertain beside a staged group")
        << QStringList{stepOf(step::uncertain), stepOf(step::staged)} << true << false << false;
}

void TestRecoveryOffers::offersFollowWhereEachGroupIs() {
    QFETCH(QStringList, steps);
    QFETCH(bool, restore);
    QFETCH(bool, retryTrash);
    QFETCH(bool, confirmTrashed);

    const application::RecoveryOffers offers =
        application::recoveryOffersFor(recordWith(steps, domain::OperationState::NeedsRecovery));

    QCOMPARE(offers.restore, restore);
    QCOMPARE(offers.retryTrash, retryTrash);
    QCOMPARE(offers.confirmTrashed, confirmTrashed);
    QCOMPARE(offers.any(), restore || retryTrash || confirmTrashed);
}

void TestRecoveryOffers::aGroupWhoseFilesDisagreeIsCountedAsNothing() {
    application::OperationRecord record = recordWith({stepOf(application::operationStep::staged)},
                                                     domain::OperationState::NeedsRecovery);
    QVERIFY(application::recoveryOffersFor(record).restore);

    // The JPG is in staging and the RAW is not. Half a group accounted for is
    // exactly the state no offer covers, so none is made.
    record.members[1].lastDurableStep = application::operationStep::planned;
    QVERIFY(!application::recoveryOffersFor(record).any());
}

void TestRecoveryOffers::aGroupTheJournalDoesNotKnowIsCountedAsNothing() {
    application::OperationRecord record = recordWith(
        {stepOf(application::operationStep::uncertain)}, domain::OperationState::NeedsRecovery);
    QVERIFY(application::recoveryOffersFor(record).confirmTrashed);

    // A planned member with no journal entry: the record cannot speak for the
    // group, so it does not.
    record.members.removeLast();
    QVERIFY(!application::recoveryOffersFor(record).any());
}

void TestRecoveryOffers::anOperationWithNoGroupsOffersNothing() {
    application::OperationRecord record;
    record.state = domain::OperationState::NeedsRecovery;
    QVERIFY(!application::recoveryOffersFor(record).any());
}

void TestRecoveryOffers::aCompletedOperationOffersNothing() {
    // Even with a journal that still says staged: a completed operation is
    // over, and offering to put its files back would be wrong.
    const application::OperationRecord record =
        recordWith({stepOf(application::operationStep::staged)}, domain::OperationState::Completed);
    QVERIFY(!application::recoveryOffersFor(record).any());
}

void TestRecoveryOffers::onlyWorkLeftOnDiskNeedsAttention_data() {
    // The state travels as an int, as the other data-driven suites here pass
    // their enums: OperationState is not a registered metatype.
    QTest::addColumn<int>("state");
    QTest::addColumn<bool>("needsAttention");

    using enum domain::OperationState;
    const auto row = [](domain::OperationState state) { return static_cast<int>(state); };

    // Nothing on the source filesystem to repair: done, stopped before
    // anything moved, or already put back.
    QTest::newRow("completed") << row(Completed) << false;
    QTest::newRow("failed") << row(Failed) << false;
    QTest::newRow("planned") << row(Planned) << false;

    // Stopped part way through, so a person has to settle it.
    QTest::newRow("staging") << row(Staging) << true;
    QTest::newRow("staged") << row(Staged) << true;
    QTest::newRow("trashing") << row(Trashing) << true;
    QTest::newRow("restoring") << row(Restoring) << true;
    QTest::newRow("needs recovery") << row(NeedsRecovery) << true;
}

void TestRecoveryOffers::onlyWorkLeftOnDiskNeedsAttention() {
    QFETCH(int, state);
    QFETCH(bool, needsAttention);

    const application::OperationRecord record = recordWith(
        {stepOf(application::operationStep::staged)}, static_cast<domain::OperationState>(state));
    QCOMPARE(application::needsRecoveryAttention(record), needsAttention);
}

QTEST_MAIN(TestRecoveryOffers)
#include "tst_recovery_offers.moc"
