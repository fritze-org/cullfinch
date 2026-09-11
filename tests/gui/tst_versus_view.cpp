// SPDX-License-Identifier: GPL-3.0-or-later
#include "GuiFixture.h"

#include <cullfinch/flows/versus/VersusFlow.h>
#include <cullfinch/views/versus/VersusView.h>

#include <QAction>
#include <QFocusEvent>
#include <QLabel>
#include <QPointF>
#include <QPushButton>
#include <QTest>

#include <tuple>

using namespace cullfinch;
using cullfinch::guitests::GuiFixture;

class TestVersusView : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void showsTwoPreviewsAndRejectsTheClickedPhoto();
    void keepButtonsEliminateTheOtherSide();
    void decisionsAreDisabledUntilBothPreviewsAreReady();
    void undoRestoresTheExactPreviousMatch();
    void aRepeatedClickOnADecidedMatchDecidesNothing();
    void spaceEntersInspectionAndAPanDoesNotEliminate();
    void losingFocusMidGestureDecidesNothing();
    void escapeLeavesFullscreenBeforePausing();
    void aDecodeErrorIsNotARejection();

private:
    void startVersusOn(int count);

    std::unique_ptr<GuiFixture> fixture_;
    views::versus::VersusView* view_ = nullptr;
    ui::ComparisonShell* shell_ = nullptr;
};

void TestVersusView::initTestCase() {
    guitests::requirePlatform(qEnvironmentVariable("CULLFINCH_EXPECTED_PLATFORM"));
}

void TestVersusView::init() {
    fixture_ = std::make_unique<GuiFixture>();
    QString error;
    QVERIFY2(fixture_->initialise(&error), qPrintable(error));

    for (int index = 1; index <= 4; ++index) {
        fixture_->collection().addJpeg(QStringLiteral("IMG_%1.JPG").arg(index));
        fixture_->collection().addRaw(QStringLiteral("IMG_%1.RAF").arg(index));
    }

    QVERIFY(fixture_->showWindow() != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(fixture_->window()));
    QVERIFY(fixture_->openCollection(4));
    view_ = nullptr;
    shell_ = nullptr;
}

void TestVersusView::cleanup() {
    view_ = nullptr;
    shell_ = nullptr;
    fixture_.reset();
}

void TestVersusView::startVersusOn(int count) {
    QList<domain::AssetId> ids;
    for (int row = 0; row < count; ++row) {
        ids.append(fixture_->window()->model()->idForRow(row));
    }
    fixture_->window()->selectAssets(ids);
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("versus-tree")));

    shell_ = fixture_->window()->activeShell();
    QVERIFY(shell_ != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(shell_));
    view_ = dynamic_cast<views::versus::VersusView*>(shell_->view());
    QVERIFY(view_ != nullptr);

    // Wait for both previews to decode, which is also what enables input. A
    // deliberately corrupt fixture never becomes ready, so this is not fatal
    // here; callers assert readiness when they depend on it.
    std::ignore = GuiFixture::waitFor([this]() {
        return (view_->leftCanvas()->isReady() || view_->leftCanvas()->hasError()) &&
               (view_->rightCanvas()->isReady() || view_->rightCanvas()->hasError());
    });
}

void TestVersusView::showsTwoPreviewsAndRejectsTheClickedPhoto() {
    startVersusOn(4);

    application::SessionController& session = fixture_->root().session();
    const flows::versus::MatchView match = flows::versus::VersusFlow::pendingMatch(session.state());
    QVERIFY(match.isValid());
    QCOMPARE(view_->leftCanvas()->presentation().id, match.left);
    QCOMPARE(view_->rightCanvas()->presentation().id, match.right);

    // Click the image to eliminate it: the same meaning as on the wall.
    QTest::mouseClick(view_->leftCanvas(), Qt::LeftButton, Qt::NoModifier,
                      view_->leftCanvas()->rect().center());

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));
    QCOMPARE(session.summary().draftRejected.first(), match.left);
    QVERIFY(!session.summary().remaining.contains(match.left));
}

void TestVersusView::keepButtonsEliminateTheOtherSide() {
    startVersusOn(4);

    application::SessionController& session = fixture_->root().session();
    const flows::versus::MatchView match = flows::versus::VersusFlow::pendingMatch(session.state());

    auto* keepLeft = shell_->findChild<QPushButton*>(QStringLiteral("versusKeepLeft"));
    QVERIFY(keepLeft != nullptr);
    QVERIFY(keepLeft->isEnabled());
    QTest::mouseClick(keepLeft, Qt::LeftButton);

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));
    // "Keep left" is the same decision as clicking the right image.
    QCOMPARE(session.summary().draftRejected.first(), match.right);
}

void TestVersusView::decisionsAreDisabledUntilBothPreviewsAreReady() {
    QList<domain::AssetId> ids;
    for (int row = 0; row < 4; ++row) {
        ids.append(fixture_->window()->model()->idForRow(row));
    }
    fixture_->window()->selectAssets(ids);
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("versus-tree")));

    shell_ = fixture_->window()->activeShell();
    view_ = dynamic_cast<views::versus::VersusView*>(shell_->view());
    QVERIFY(view_ != nullptr);

    auto* keepLeft = shell_->findChild<QPushButton*>(QStringLiteral("versusKeepLeft"));
    QVERIFY(keepLeft != nullptr);
    // Before both previews arrive, nobody can judge the pair.
    if (!view_->leftCanvas()->isReady() || !view_->rightCanvas()->isReady()) {
        QVERIFY(!keepLeft->isEnabled());
    }

    QVERIFY(GuiFixture::waitFor(
        [this]() { return view_->leftCanvas()->isReady() && view_->rightCanvas()->isReady(); }));
    QVERIFY(keepLeft->isEnabled());
}

void TestVersusView::undoRestoresTheExactPreviousMatch() {
    startVersusOn(4);

    application::SessionController& session = fixture_->root().session();
    const flows::versus::MatchView first = flows::versus::VersusFlow::pendingMatch(session.state());

    QTest::mouseClick(view_->rightCanvas(), Qt::LeftButton, Qt::NoModifier,
                      view_->rightCanvas()->rect().center());
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));

    auto* undo = shell_->findChild<QAction*>(QStringLiteral("comparisonUndo"));
    QVERIFY(undo != nullptr);
    QVERIFY(undo->isEnabled());
    undo->trigger();

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.isEmpty(); }));

    const flows::versus::MatchView restored =
        flows::versus::VersusFlow::pendingMatch(session.state());
    QCOMPARE(restored.node, first.node);
    QCOMPARE(restored.left, first.left);
    QCOMPARE(restored.right, first.right);

    // A different choice now discards the abandoned redo branch.
    auto* redo = shell_->findChild<QAction*>(QStringLiteral("comparisonRedo"));
    QVERIFY(redo != nullptr);
    QVERIFY(redo->isEnabled());
    QVERIFY(GuiFixture::waitFor(
        [this]() { return view_->leftCanvas()->isReady() && view_->rightCanvas()->isReady(); }));
    QTest::mouseClick(view_->leftCanvas(), Qt::LeftButton, Qt::NoModifier,
                      view_->leftCanvas()->rect().center());
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));
    QCOMPARE(session.summary().draftRejected.first(), first.left);
    QVERIFY(!redo->isEnabled());
}

void TestVersusView::aRepeatedClickOnADecidedMatchDecidesNothing() {
    startVersusOn(4);
    application::SessionController& session = fixture_->root().session();

    ui::ImageCanvas* left = view_->leftCanvas();
    const QPoint centre = left->rect().center();

    // Two clicks in immediate succession at the same place: one decision.
    QTest::mouseClick(left, Qt::LeftButton, Qt::NoModifier, centre);
    QTest::mouseClick(left, Qt::LeftButton, Qt::NoModifier, centre);

    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() >= 1; }));
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 1);
}

void TestVersusView::spaceEntersInspectionAndAPanDoesNotEliminate() {
    startVersusOn(4);
    application::SessionController& session = fixture_->root().session();

    ui::ImageCanvas* left = view_->leftCanvas();
    left->setFocus();
    QTest::keyClick(left, Qt::Key_Space);
    QVERIFY(left->isInspecting());

    // Click-and-drag pans; it is not an elimination.
    const QPoint start = left->rect().center();
    QTest::mousePress(left, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(left, start + QPoint(40, 25));
    QTest::mouseRelease(left, Qt::LeftButton, Qt::NoModifier, start + QPoint(40, 25));

    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 0);

    // A plain click while inspecting is still not a decision.
    QTest::mouseClick(left, Qt::LeftButton, Qt::NoModifier, start);
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 0);

    QTest::keyClick(left, Qt::Key_Space);
    QVERIFY(!left->isInspecting());
}

void TestVersusView::losingFocusMidGestureDecidesNothing() {
    startVersusOn(4);
    application::SessionController& session = fixture_->root().session();

    ui::ImageCanvas* left = view_->leftCanvas();
    const QPoint centre = left->rect().center();

    // Alt-Tab, a workspace switch, a dialog or the lock screen all arrive as a
    // focus loss between press and release. The half-finished gesture must not
    // become a decision about a photo the user stopped looking at.
    //
    // The focus-out event is delivered directly rather than by moving focus to
    // another widget: a headless compositor can have no input seat, so whether
    // setFocus() is honoured depends on the backend. What this suite owns is
    // the widget's response to losing focus. Real focus routing belongs to the
    // compositor integration suite.
    QTest::mousePress(left, Qt::LeftButton, Qt::NoModifier, centre);
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QCoreApplication::sendEvent(left, &focusOut);
    QTest::mouseRelease(left, Qt::LeftButton, Qt::NoModifier, centre);

    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 0);

    // A fresh, uninterrupted gesture still decides.
    QVERIFY(GuiFixture::waitFor([this]() { return view_->leftCanvas()->isReady(); }));
    QTest::mouseClick(left, Qt::LeftButton, Qt::NoModifier, centre);
    QVERIFY(GuiFixture::waitFor([&]() { return session.summary().draftRejected.size() == 1; }));
}

void TestVersusView::escapeLeavesFullscreenBeforePausing() {
    startVersusOn(4);
    application::SessionController& session = fixture_->root().session();

    auto* fullscreen = shell_->findChild<QAction*>(QStringLiteral("comparisonFullscreen"));
    QVERIFY(fullscreen != nullptr);
    fullscreen->trigger();
    QVERIFY(GuiFixture::waitFor([&]() { return shell_->isFullScreen(); }, 5000));

    // Escape leaves fullscreen first, and does not end the session.
    QTest::keyClick(shell_, Qt::Key_Escape);
    QVERIFY(GuiFixture::waitFor([&]() { return !shell_->isFullScreen(); }, 5000));
    QVERIFY(session.isActive());

    // A second Escape pauses and returns to the browser, applying nothing.
    QTest::keyClick(shell_, Qt::Key_Escape);
    QVERIFY(GuiFixture::waitFor([&]() { return !session.isActive(); }));
    QCOMPARE(fixture_->root().collection().rejectedCount(), 0);
}

void TestVersusView::aDecodeErrorIsNotARejection() {
    // A corrupt JPG produces a visible error, never an automatic RAW fallback
    // and never a rejection decision.
    fixture_->collection().addCorruptJpeg(QStringLiteral("IMG_1.JPG"));
    fixture_->root().collection().refresh();
    QVERIFY(GuiFixture::waitFor([&]() { return fixture_->window()->model()->rowCount() == 4; }));

    startVersusOn(4);
    application::SessionController& session = fixture_->root().session();

    ui::ImageCanvas* broken = nullptr;
    if (view_->leftCanvas()->hasError()) {
        broken = view_->leftCanvas();
    } else if (view_->rightCanvas()->hasError()) {
        broken = view_->rightCanvas();
    }
    if (broken == nullptr) {
        QSKIP("the corrupt fixture was not part of the first match");
    }

    QTest::mouseClick(broken, Qt::LeftButton, Qt::NoModifier, broken->rect().center());
    QCoreApplication::processEvents();
    QCOMPARE(session.summary().draftRejected.size(), 0);
    QVERIFY(!broken->errorText().isEmpty());
}

QTEST_MAIN(TestVersusView)
#include "tst_versus_view.moc"
