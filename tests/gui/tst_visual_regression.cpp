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
// What is left over is recorded per rendering environment instead: the
// browser's status bar, which is nothing but text, and its grid, whose cells
// are a fixed size but split between thumbnail and label by the label's own
// height. See VisualBaseline.h.
#include "GuiFixture.h"
#include "VisualBaseline.h"

#include <cullfinch/ui/AssetListModel.h>
#include <cullfinch/ui/FlowView.h>
#include <cullfinch/views/versus/VersusView.h>
#include <cullfinch/views/wall/WallView.h>

#include <QAbstractItemModel>
#include <QColor>
#include <QGuiApplication>
#include <QListView>
#include <QPainter>
#include <QStatusBar>
#include <QTest>

#include <array>
#include <cstddef>
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
    constexpr std::array colours{qRgb(0xC0, 0x39, 0x2B), qRgb(0x27, 0xAE, 0x60),
                                 qRgb(0x29, 0x80, 0xB9), qRgb(0xF1, 0xC4, 0x0F),
                                 qRgb(0x8E, 0x44, 0xAD), qRgb(0x16, 0xA0, 0x85)};
    for (std::size_t index = 0; index < colours.size(); ++index) {
        const QSize size = (index % 2 == 0) ? QSize(320, 240) : QSize(240, 320);
        const int number = static_cast<int>(index) + 1;
        collection.addSolidJpeg(QStringLiteral("IMG_%1.JPG").arg(number), QColor(colours.at(index)),
                                size);
        collection.addRaw(QStringLiteral("IMG_%1.RAF").arg(number));
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
    // These report failure by returning null rather than through QVERIFY: a
    // QVERIFY inside a helper returns from the helper, and leaves the test
    // carrying on with whatever it was handed.
    [[nodiscard]] ui::IFlowView* startFlowOn(const QString& flowId, int count);
    [[nodiscard]] views::wall::WallSurface* startWallOn(int count);
    [[nodiscard]] views::versus::VersusView* startVersusOn(int count);
    [[nodiscard]] static QImage renderStatusBar(const ui::BrowserWindow* window);
    [[nodiscard]] static QImage renderWallSurface(views::wall::WallSurface* surface);

    std::unique_ptr<GuiFixture> fixture_;
    std::unique_ptr<VisualBaseline> baseline_;
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
    fixture_.reset();
}

ui::IFlowView* TestVisualRegression::startFlowOn(const QString& flowId, int count) {
    ui::BrowserWindow* window = fixture_->window();
    ui::AssetListModel* model = (window != nullptr) ? window->model() : nullptr;
    if (model == nullptr) {
        return nullptr;
    }

    QList<domain::AssetId> ids;
    for (int row = 0; row < count; ++row) {
        ids.append(model->idForRow(row));
    }
    window->selectAssets(ids);
    if (!window->startFlow(flowId)) {
        return nullptr;
    }

    ui::ComparisonShell* shell = window->activeShell();
    if (shell == nullptr) {
        return nullptr;
    }
    guitests::settleWindow(shell);
    return shell->view();
}

views::wall::WallSurface* TestVisualRegression::startWallOn(int count) {
    auto* view =
        dynamic_cast<views::wall::WallView*>(startFlowOn(QStringLiteral("image-wall"), count));
    return (view != nullptr) ? view->surface() : nullptr;
}

views::versus::VersusView* TestVisualRegression::startVersusOn(int count) {
    return dynamic_cast<views::versus::VersusView*>(
        startFlowOn(QStringLiteral("versus-tree"), count));
}

QImage TestVisualRegression::renderStatusBar(const ui::BrowserWindow* window) {
    QStatusBar* status = window->statusBar();
    if (status == nullptr) {
        return {};
    }

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

QImage TestVisualRegression::renderWallSurface(views::wall::WallSurface* surface) {
    if (surface == nullptr || !guitests::pinSize(surface, kWallSurfaceSize)) {
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
    if (grid == nullptr) {
        QFAIL("the browser has no asset grid");
    }
    QVERIFY(guitests::pinSize(grid, kGridSize));

    // The viewport, not the view: the frame and the scroll bars belong to the
    // style, and the tiles are what the browser lays out.
    QImage rendering = guitests::renderSettled(grid->viewport());
    QVERIFY2(!rendering.isNull(), "the browser grid never stopped changing");

    // Masking the label band drops the glyphs, but not the font: the cell is a
    // fixed size and the delegate splits it between decoration and text by the
    // text's own height, so a taller font draws a smaller thumbnail. That is
    // what keeps this case per-environment while the wall and versus ones are
    // shared.
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
                                VisualScope::FontDependent);
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
    if (!window) {
        QFAIL("the second composition built no browser window");
    }
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
    views::wall::WallSurface* surface = startWallOn(6);
    if (surface == nullptr) {
        QFAIL("the wall flow did not start");
    }

    const QImage rendering = renderWallSurface(surface);
    QVERIFY2(!rendering.isNull(), "the wall surface never stopped changing");

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("wall-surface"), rendering,
                                VisualScope::FontIndependent);
}

void TestVisualRegression::theWallIsUnchangedAfterAnElimination() {
    views::wall::WallSurface* surface = startWallOn(6);
    if (surface == nullptr) {
        QFAIL("the wall flow did not start");
    }
    QVERIFY(guitests::pinSize(surface, kWallSurfaceSize));

    application::SessionController& session = fixture_->root().session();
    const domain::AssetId victim = surface->order().at(2);
    ui::ImageCanvas* tile = surface->tileFor(victim);
    if (tile == nullptr) {
        QFAIL("the wall has no tile for the photo it is about to eliminate");
    }
    QTest::mouseClick(tile, Qt::LeftButton, Qt::NoModifier, tile->rect().center());
    QVERIFY(GuiFixture::waitFor([&session]() { return session.summary().remaining.size() == 5; }));

    // Five survivors reflowed into the same surface: a different grid, and the
    // case that catches a reflow regression a count assertion cannot see.
    const QImage rendering = renderWallSurface(surface);
    QVERIFY2(!rendering.isNull(), "the wall surface never stopped changing");

    CULLFINCH_COMPARE_RENDERING(QStringLiteral("wall-surface-after-elimination"), rendering,
                                VisualScope::FontIndependent);
}

void TestVisualRegression::theVersusPanesAreUnchanged() {
    views::versus::VersusView* view = startVersusOn(4);
    if (view == nullptr) {
        QFAIL("the versus flow did not start");
    }

    ui::ImageCanvas* left = view->leftCanvas();
    ui::ImageCanvas* right = view->rightCanvas();
    if (left == nullptr || right == nullptr) {
        QFAIL("the versus view did not build both panes");
    }

    // Equal available area is the versus layout's promise. It is asserted here
    // rather than read out of the pixels, because the panes are about to be
    // pinned to a stated size.
    QVERIFY(GuiFixture::waitFor(
        [left, right]() { return left->preview()->isReady() && right->preview()->isReady(); }));
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
