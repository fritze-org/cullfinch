// SPDX-License-Identifier: GPL-3.0-or-later
//
// The behavioural GUI suites prove that the right signals reach the right
// controllers. None of them looks at what the user sees, so a stylesheet, a
// layout margin or a paint order that silently moves half the view past them
// unnoticed. This suite renders the production widgets offscreen and compares
// the pixels against a checked-in reference.
//
// Two rules keep that from becoming a source of noise:
//
//   * every compared widget is pinned to a size the case states, so a
//     reference records the view rather than the surrounding chrome's font
//     metrics, and
//   * every region whose content is glyphs is masked, so the comparison keeps
//     the geometry that decides where the text goes and drops the
//     rasterisation, which no two font stacks agree on.
//
// What is left over -- the browser's status bar, which is nothing but text --
// is recorded per rendering environment instead. See VisualBaseline.h.
#include "GuiFixture.h"
#include "VisualBaseline.h"

#include <cullfinch/views/versus/VersusView.h>
#include <cullfinch/views/wall/WallView.h>

#include <QAbstractItemModel>
#include <QColor>
#include <QGuiApplication>
#include <QListView>
#include <QPainter>
#include <QStatusBar>
#include <QTest>

#include <cstdlib>
#include <memory>
#include <tuple>

using namespace cullfinch;
using cullfinch::guitests::GuiFixture;
using cullfinch::guitests::VisualBaseline;
using cullfinch::guitests::VisualScope;
using cullfinch::guitests::VisualTolerance;

namespace {

// The sizes every case renders at. They are arbitrary but fixed: what matters
// is that a reference and the rendering it is compared against were taken at
// the same one, whatever the window manager or the font would have chosen.
const QSize kGridSize(840, 480);
const QSize kWallSurfaceSize(880, 560);
const QSize kVersusPaneSize(400, 300);
const QSize kStatusBarSize(560, 40);

/// Gap drawn between the two versus panes in the composed rendering. Not a
/// property of the view: it just keeps the two panes visually separate in a
/// reference someone opens to look at.
constexpr int kVersusPaneGap = 8;

/// The caption strip ImageCanvas pins to the bottom of every photo.
constexpr int kCaptionStripHeight = 24;

/// The band of a browser tile the delegate draws the display name in. A tile
/// is 196x210 with a 160x160 icon, so the bottom fifty-odd pixels are label.
constexpr int kTileLabelBandHeight = 52;

/// Six flat photos, alternating landscape and portrait so the wall's fit
/// policy is exercised, and carrying no drawn text of their own.
void addFlatPhotos(testsupport::TempCollection& collection) {
    const QList<QRgb> colours{qRgb(0xC0, 0x39, 0x2B), qRgb(0x27, 0xAE, 0x60),
                              qRgb(0x29, 0x80, 0xB9), qRgb(0xF1, 0xC4, 0x0F),
                              qRgb(0x8E, 0x44, 0xAD), qRgb(0x16, 0xA0, 0x85)};
    for (int index = 0; index < colours.size(); ++index) {
        const QSize size = (index % 2 == 0) ? QSize(320, 240) : QSize(240, 320);
        collection.addSolidJpeg(QStringLiteral("IMG_%1.JPG").arg(index + 1),
                                QColor(colours.at(index)), size);
        collection.addRaw(QStringLiteral("IMG_%1.RAF").arg(index + 1));
    }
}

} // namespace

/// Compare a rendering, skipping the case when this environment has no
/// reference set at all and failing it when the rendering moved.
///
/// A macro rather than a helper because QSKIP and QVERIFY2 both act by
/// returning from the function they are written in, which has to be the test.
#define CULLFINCH_COMPARE_RENDERING(name, rendering, scope)                                        \
    do {                                                                                           \
        QString report;                                                                            \
        const VisualBaseline::Outcome outcome =                                                    \
            baseline_->compare((name), (rendering), (scope), VisualTolerance{}, &report);          \
        if (outcome == VisualBaseline::Outcome::Unrecorded) {                                      \
            QSKIP(qPrintable(report));                                                             \
        }                                                                                          \
        QVERIFY2(outcome != VisualBaseline::Outcome::Differed, qPrintable(report));                \
    } while (false)

class TestVisualRegression : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void theBrowserGridLaysOutItsTilesUnchanged();
    void theBrowserStatusBarIsUnchangedWhileWritable();
    void theBrowserStatusBarShowsTheReadOnlyIndicator();
    void theWallLaysOutItsTilesUnchanged();
    void theWallIsUnchangedAfterAnElimination();
    void theVersusPanesAreUnchanged();

private:
    void startWallOn(int count);
    void startVersusOn(int count);
    [[nodiscard]] QImage renderStatusBar(ui::BrowserWindow* window) const;
    [[nodiscard]] QImage renderWallSurface() const;

    std::unique_ptr<GuiFixture> fixture_;
    std::unique_ptr<VisualBaseline> baseline_;
    views::wall::WallView* wall_ = nullptr;
    views::versus::VersusView* versus_ = nullptr;
};

void TestVisualRegression::initTestCase() {
    // Unlike the behavioural GUI suites, this one is registered with an
    // explicit platform: a compositor decides how a surface is composed, and
    // comparing pixels only means something against the backend the reference
    // was taken on. Cullfinch's own widget rendering is what is under test
    // here, not its desktop presentation.
    QVERIFY2(QGuiApplication::platformName() == QStringLiteral("offscreen"),
             "the visual suite compares pixels and runs on the offscreen backend only; run it "
             "through ctest, which selects the platform plugin for it");

    VisualBaseline::pinAppearance();
    baseline_ = std::make_unique<VisualBaseline>();
    qInfo("visual rendering environment %s\n%s", qPrintable(baseline_->environmentId()),
          qPrintable(baseline_->environmentReport()));
}

void TestVisualRegression::init() {
    fixture_ = std::make_unique<GuiFixture>();
    QString error;
    QVERIFY2(fixture_->initialise(&error), qPrintable(error));

    addFlatPhotos(fixture_->collection());

    QVERIFY(fixture_->showWindow() != nullptr);
    guitests::settleWindow(fixture_->window());
    QVERIFY(fixture_->openCollection(6));
}

void TestVisualRegression::cleanup() {
    wall_ = nullptr;
    versus_ = nullptr;
    fixture_.reset();
}

void TestVisualRegression::startWallOn(int count) {
    QList<domain::AssetId> ids;
    for (int row = 0; row < count; ++row) {
        ids.append(fixture_->window()->model()->idForRow(row));
    }
    fixture_->window()->selectAssets(ids);
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("image-wall")));

    ui::ComparisonShell* shell = fixture_->window()->activeShell();
    QVERIFY(shell != nullptr);
    guitests::settleWindow(shell);
    wall_ = dynamic_cast<views::wall::WallView*>(shell->view());
    QVERIFY(wall_ != nullptr);
}

void TestVisualRegression::startVersusOn(int count) {
    QList<domain::AssetId> ids;
    for (int row = 0; row < count; ++row) {
        ids.append(fixture_->window()->model()->idForRow(row));
    }
    fixture_->window()->selectAssets(ids);
    QVERIFY(fixture_->window()->startFlow(QStringLiteral("versus-tree")));

    ui::ComparisonShell* shell = fixture_->window()->activeShell();
    QVERIFY(shell != nullptr);
    guitests::settleWindow(shell);
    versus_ = dynamic_cast<views::versus::VersusView*>(shell->view());
    QVERIFY(versus_ != nullptr);
}

QImage TestVisualRegression::renderStatusBar(ui::BrowserWindow* window) const {
    QStatusBar* status = window->statusBar();

    // Temporary messages carry a scan's progress and, when the collection was
    // downgraded, the identity of the process holding the lock. Neither is a
    // property of the layout under test, and the second is different on every
    // run. The permanent widgets -- the counts and the read-only indicator --
    // are what this case is about, and clearMessage() does not touch them.
    std::ignore =
        GuiFixture::waitFor([status]() { return status->currentMessage().isEmpty(); }, 12000);
    status->clearMessage();

    if (!guitests::pinSize(status, kStatusBarSize)) {
        return {};
    }
    return guitests::renderSettled(status);
}

QImage TestVisualRegression::renderWallSurface() const {
    views::wall::WallSurface* surface = wall_->surface();
    if (!guitests::pinSize(surface, kWallSurfaceSize)) {
        return {};
    }

    QImage rendering = guitests::renderSettled(surface);
    if (rendering.isNull()) {
        return rendering;
    }

    // Every tile names its photo in a strip pinned to its own bottom edge, so
    // the mask follows the tile the layout produced.
    for (const domain::AssetId& id : surface->order()) {
        const ui::ImageCanvas* tile = surface->tileFor(id);
        if (tile == nullptr) {
            continue;
        }
        const QRect geometry = tile->geometry();
        guitests::maskRegion(rendering,
                             QRect(geometry.left(), geometry.bottom() - kCaptionStripHeight + 1,
                                   geometry.width(), kCaptionStripHeight));
    }
    return rendering;
}

void TestVisualRegression::theBrowserGridLaysOutItsTilesUnchanged() {
    auto* grid = fixture_->window()->findChild<QListView*>(QStringLiteral("assetGrid"));
    QVERIFY(grid != nullptr);
    QVERIFY(guitests::pinSize(grid, kGridSize));

    // The viewport, not the view: the frame and the scroll bars belong to the
    // style, and the tiles are what the browser lays out.
    QImage rendering = guitests::renderSettled(grid->viewport());
    QVERIFY2(!rendering.isNull(), "the browser grid never stopped changing");

    const QAbstractItemModel* model = grid->model();
    QCOMPARE(model->rowCount(), 6);
    for (int row = 0; row < model->rowCount(); ++row) {
        const QRect cell = grid->visualRect(model->index(row, 0));
        if (cell.isEmpty()) {
            continue;
        }
        guitests::maskRegion(rendering, QRect(cell.left(), cell.bottom() - kTileLabelBandHeight + 1,
                                              cell.width(), kTileLabelBandHeight));
    }

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("browser-grid"), rendering,
                                VisualScope::FontIndependent);
}

void TestVisualRegression::theBrowserStatusBarIsUnchangedWhileWritable() {
    QVERIFY(!fixture_->root().collection().isReadOnly());

    const QImage rendering = renderStatusBar(fixture_->window());
    QVERIFY2(!rendering.isNull(), "the browser status bar never stopped changing");

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("browser-status-bar-writable"), rendering,
                                VisualScope::FontDependent);
}

void TestVisualRegression::theBrowserStatusBarShowsTheReadOnlyIndicator() {
    // The fixture's window holds the writer lock, so a second composition
    // against the same application data root is exactly a second launch, and
    // opens the collection read-only.
    app::CompositionRoot::Options options;
    options.dataDirectory = fixture_->dataDirectory();
    options.cacheDirectory = fixture_->cacheDirectory();
    options.trashAdapter = &fixture_->trash();
    app::CompositionRoot second(options);
    QString error;
    QVERIFY2(second.initialise(&error), qPrintable(error));

    std::unique_ptr<ui::BrowserWindow> window(second.createBrowserWindow());
    window->show();
    guitests::settleWindow(window.get());
    QVERIFY(window->openDirectory(fixture_->collection().path()));
    QVERIFY(second.collection().isReadOnly());

    const QImage rendering = renderStatusBar(window.get());
    QVERIFY2(!rendering.isNull(), "the browser status bar never stopped changing");

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("browser-status-bar-read-only"), rendering,
                                VisualScope::FontDependent);
}

void TestVisualRegression::theWallLaysOutItsTilesUnchanged() {
    startWallOn(6);

    const QImage rendering = renderWallSurface();
    QVERIFY2(!rendering.isNull(), "the wall surface never stopped changing");

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("wall-surface"), rendering,
                                VisualScope::FontIndependent);
}

void TestVisualRegression::theWallIsUnchangedAfterAnElimination() {
    startWallOn(6);
    views::wall::WallSurface* surface = wall_->surface();
    QVERIFY(guitests::pinSize(surface, kWallSurfaceSize));

    application::SessionController& session = fixture_->root().session();
    const domain::AssetId victim = surface->order().at(2);
    ui::ImageCanvas* tile = surface->tileFor(victim);
    QVERIFY(tile != nullptr);
    QTest::mouseClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());
    QVERIFY(GuiFixture::waitFor([&session]() { return session.summary().remaining.size() == 5; }));

    // Five survivors reflowed into the same surface: a different grid, and the
    // case that catches a reflow regression a count assertion cannot see.
    const QImage rendering = renderWallSurface();
    QVERIFY2(!rendering.isNull(), "the wall surface never stopped changing");

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("wall-surface-after-elimination"), rendering,
                                VisualScope::FontIndependent);
}

void TestVisualRegression::theVersusPanesAreUnchanged() {
    startVersusOn(4);

    ui::ImageCanvas* left = versus_->leftCanvas();
    ui::ImageCanvas* right = versus_->rightCanvas();
    QVERIFY(left != nullptr && right != nullptr);

    // Equal available area is the versus layout's promise. It is asserted
    // here rather than read out of the pixels, because the panes are about to
    // be pinned to a stated size.
    QVERIFY(GuiFixture::waitFor([left, right]() { return left->isReady() && right->isReady(); }));
    QVERIFY2(std::abs(left->width() - right->width()) <= 1 && left->height() == right->height(),
             "the versus panes are not laid out at equal area");

    QVERIFY(guitests::pinSize(left, kVersusPaneSize));
    QVERIFY(guitests::pinSize(right, kVersusPaneSize));
    const QImage leftPane = guitests::renderSettled(left);
    const QImage rightPane = guitests::renderSettled(right);
    QVERIFY2(!leftPane.isNull() && !rightPane.isNull(), "a versus pane never stopped changing");

    QImage rendering(QSize(kVersusPaneSize.width() * 2 + kVersusPaneGap, kVersusPaneSize.height()),
                     QImage::Format_RGB32);
    rendering.fill(Qt::black);
    QPainter painter(&rendering);
    painter.drawImage(QPoint(0, 0), leftPane);
    painter.drawImage(QPoint(kVersusPaneSize.width() + kVersusPaneGap, 0), rightPane);
    painter.end();

    for (int pane = 0; pane < 2; ++pane) {
        const int originX = pane * (kVersusPaneSize.width() + kVersusPaneGap);
        guitests::maskRegion(rendering,
                             QRect(originX, kVersusPaneSize.height() - kCaptionStripHeight,
                                   kVersusPaneSize.width(), kCaptionStripHeight));
    }

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("versus-panes"), rendering,
                                VisualScope::FontIndependent);
}

QTEST_MAIN(TestVisualRegression)
#include "tst_visual_regression.moc"
