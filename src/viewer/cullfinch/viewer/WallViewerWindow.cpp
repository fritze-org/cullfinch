// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/viewer/WallViewerWindow.h>

#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QKeySequence>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>

namespace cullfinch::viewer {
namespace {

/// One notch of Ctrl+wheel, Ctrl++ or Ctrl+-. Multiplicative, so a step feels
/// the same at 100 pixels as at 800.
constexpr double kZoomFactor = 1.25;
/// Qt's wheel unit: 120 per notch of a conventional mouse wheel.
constexpr int kWheelNotch = 120;
/// What a photo's tile gets before any decode says how wide it is.
constexpr int kFallbackTileWidth = 256;

/// A width within the slider's range, or zero for Fit.
int clampedTileWidth(int pixels) {
    return pixels > 0 ? std::clamp(pixels, WallViewerWindow::kMinimumTileWidth,
                                   WallViewerWindow::kMaximumTileWidth)
                      : 0;
}

/// A platform's standard bindings plus extras, each sequence once. A QAction
/// that owns one sequence twice treats the key as ambiguous and triggers on
/// neither, and the extras here are the standard binding on some platforms.
QList<QKeySequence> shortcutsFor(QKeySequence::StandardKey key,
                                 std::initializer_list<QKeySequence> extras) {
    QList<QKeySequence> sequences = QKeySequence::keyBindings(key);
    for (const QKeySequence& extra : extras) {
        if (!extra.isEmpty() && !sequences.contains(extra)) {
            sequences.append(extra);
        }
    }
    return sequences;
}

} // namespace

WallViewerWindow::WallViewerWindow(application::IImageService& images, const QString& directory,
                                   const DirectoryPhotos& photos, int tileWidth, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("wallViewerWindow"));
    // The title names what the count covers: the same number means something
    // else for a folder than for a whole tree below it.
    const QString scope = photos.recursive ? tr("%1 and subfolders").arg(QDir(directory).dirName())
                                           : QDir(directory).dirName();
    setWindowTitle(
        tr("%1 — %n photo(s)", nullptr, static_cast<int>(photos.photos.size())).arg(scope));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    strip_ = new QToolBar(this);
    strip_->setObjectName(QStringLiteral("viewerStrip"));
    layout->addWidget(strip_);

    surface_ = new views::wall::WallSurface(images);
    // Before the photos arrive, so tiles off screen are created holding back
    // their decode instead of all asking at once.
    surface_->setTileWidth(clampedTileWidth(tileWidth));

    scroll_ = new QScrollArea(this);
    scroll_->setObjectName(QStringLiteral("viewerScrollArea"));
    // The surface follows the viewport's width and asks for the height its
    // grid needs; the grid never scrolls sideways.
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidget(surface_);
    scroll_->viewport()->installEventFilter(this);
    layout->addWidget(scroll_, 1);

    emptyLabel_ =
        new QLabel(photos.recursive ? tr("There are no photos in this directory or below it.")
                                    : tr("There are no photos in this directory."),
                   this);
    emptyLabel_->setObjectName(QStringLiteral("viewerEmptyLabel"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(emptyLabel_, 1);

    const bool empty = photos.photos.isEmpty();
    scroll_->setVisible(!empty);
    emptyLabel_->setVisible(empty);

    // Every photo takes a wall position of its own and none is ever marked as
    // eliminated: this wall has no flow to eliminate anything with. Nothing is
    // connected to the surface's elimination request for the same reason.
    QList<flows::wall::WallSlot> positions;
    ui::AssetPresentationMap presentations;
    positions.reserve(photos.photos.size());
    for (const ui::AssetPresentation& photo : photos.photos) {
        positions.append(flows::wall::WallSlot{photo.id, false});
        presentations.insert(photo.id, photo);
    }
    surface_->setCandidates(positions, presentations, 1);

    buildControls(photos);
    syncControls();
}

void WallViewerWindow::buildControls(const DirectoryPhotos& photos) {
    // Every action lives on the window as well as on the strip, so its
    // shortcut keeps working while fullscreen hides the strip.
    smallerAction_ = strip_->addAction(tr("Smaller"));
    smallerAction_->setObjectName(QStringLiteral("viewerSmaller"));
    smallerAction_->setShortcuts(
        shortcutsFor(QKeySequence::ZoomOut, {QKeySequence(Qt::CTRL | Qt::Key_Minus)}));
    connect(smallerAction_, &QAction::triggered, this,
            [this]() { zoomTiles(-1, scroll_->viewport()->rect().center()); });

    size_ = new QSlider(Qt::Horizontal, strip_);
    size_->setObjectName(QStringLiteral("viewerTileSize"));
    size_->setRange(kMinimumTileWidth, kMaximumTileWidth);
    size_->setSingleStep(16);
    size_->setPageStep(64);
    size_->setMaximumWidth(240);
    size_->setToolTip(tr("Tile size"));
    size_->setAccessibleName(tr("Tile size"));
    strip_->addWidget(size_);
    connect(size_, &QSlider::valueChanged, this, [this](int value) {
        if (syncing_) {
            return;
        }
        applyTileWidth(value, scroll_->viewport()->rect().center());
    });

    largerAction_ = strip_->addAction(tr("Larger"));
    largerAction_->setObjectName(QStringLiteral("viewerLarger"));
    // Ctrl+= as well, because on most layouts "+" needs Shift.
    largerAction_->setShortcuts(
        shortcutsFor(QKeySequence::ZoomIn, {QKeySequence(Qt::CTRL | Qt::Key_Equal)}));
    connect(largerAction_, &QAction::triggered, this,
            [this]() { zoomTiles(1, scroll_->viewport()->rect().center()); });

    fitAction_ = strip_->addAction(tr("Fit to window"));
    fitAction_->setObjectName(QStringLiteral("viewerFit"));
    fitAction_->setCheckable(true);
    fitAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    connect(fitAction_, &QAction::triggered, this, [this](bool checked) {
        applyTileWidth(checked ? 0 : effectiveTileWidth(), scroll_->viewport()->rect().center());
    });

    strip_->addSeparator();
    fullscreenAction_ = strip_->addAction(tr("Fullscreen"));
    fullscreenAction_->setObjectName(QStringLiteral("viewerFullscreen"));
    fullscreenAction_->setCheckable(true);
    fullscreenAction_->setShortcuts(
        shortcutsFor(QKeySequence::FullScreen, {QKeySequence(Qt::Key_F11)}));
    connect(fullscreenAction_, &QAction::triggered, this, &WallViewerWindow::toggleFullscreen);

    addActions({smallerAction_, largerAction_, fitAction_, fullscreenAction_});

    if (photos.withoutPreview > 0) {
        // Left off the wall because there is nothing to draw, but said so: a
        // viewer that silently shows fewer photos than the folder holds looks
        // like one that lost some.
        strip_->addSeparator();
        auto* hidden = new QLabel(
            tr("%n RAW-only photo(s) not shown", nullptr, photos.withoutPreview), strip_);
        hidden->setObjectName(QStringLiteral("viewerHiddenCount"));
        strip_->addWidget(hidden);
    }
}

int WallViewerWindow::tileWidth() const {
    return surface_->tileWidth();
}

void WallViewerWindow::setTileWidth(int pixels) {
    applyTileWidth(pixels, scroll_->viewport()->rect().center());
}

int WallViewerWindow::effectiveTileWidth() const {
    if (surface_->tileWidth() > 0) {
        return surface_->tileWidth();
    }
    const QList<domain::AssetId> order = surface_->order();
    const ui::ImageCanvas* tile = order.isEmpty() ? nullptr : surface_->tileFor(order.first());
    return tile != nullptr && tile->width() > 0 ? tile->width() : kFallbackTileWidth;
}

void WallViewerWindow::zoomTiles(int steps, const QPoint& anchor) {
    if (steps == 0) {
        return;
    }
    const double scaled = effectiveTileWidth() * std::pow(kZoomFactor, steps);
    applyTileWidth(static_cast<int>(std::lround(scaled)), anchor);
}

void WallViewerWindow::applyTileWidth(int pixels, const QPoint& anchor) {
    pixels = clampedTileWidth(pixels);
    if (pixels == surface_->tileWidth()) {
        syncControls();
        return;
    }

    // The photo under the anchor, and how far down it the anchor is. Every
    // tile moves when the grid changes shape, columns included; this one is
    // scrolled back level with the anchor, so the photo somebody was looking
    // at stays on screen at the height they were looking at. The anchor can
    // fall in the spacing between two tiles, so the nearest one counts.
    const QPoint onSurface = surface_->mapFrom(scroll_->viewport(), anchor);
    domain::AssetId anchored;
    double fraction = 0.0;
    int nearest = std::numeric_limits<int>::max();
    for (const domain::AssetId& id : surface_->order()) {
        const ui::ImageCanvas* tile = surface_->tileFor(id);
        if (tile == nullptr || tile->height() <= 0) {
            continue;
        }
        const QRect cell = tile->geometry();
        const QPoint closest(std::clamp(onSurface.x(), cell.left(), cell.right()),
                             std::clamp(onSurface.y(), cell.top(), cell.bottom()));
        if (const int distance = (closest - onSurface).manhattanLength(); distance < nearest) {
            nearest = distance;
            anchored = id;
            fraction = std::clamp(static_cast<double>(onSurface.y() - cell.top()) / cell.height(),
                                  0.0, 1.0);
        }
    }

    surface_->setTileWidth(pixels);
    syncControls();
    Q_EMIT tileWidthChanged(pixels);

    if (!anchored.isValid()) {
        return;
    }
    // The scroll area takes the surface's new height when it handles the
    // layout request the width change posted, and only then can the scroll bar
    // reach the anchored photo's new position. The photo is looked up again by
    // its identity then, rather than held onto in between.
    QTimer::singleShot(0, this, [this, anchored, fraction, anchor]() {
        const ui::ImageCanvas* tile = surface_->tileFor(anchored);
        if (tile == nullptr) {
            return;
        }
        const int target = tile->y() + static_cast<int>(std::lround(fraction * tile->height()));
        scroll_->verticalScrollBar()->setValue(target - anchor.y());
    });
}

void WallViewerWindow::syncControls() {
    syncing_ = true;
    const bool fitted = surface_->tileWidth() == 0;
    fitAction_->setChecked(fitted);
    size_->setValue(std::clamp(effectiveTileWidth(), kMinimumTileWidth, kMaximumTileWidth));
    smallerAction_->setEnabled(fitted || surface_->tileWidth() > kMinimumTileWidth);
    largerAction_->setEnabled(fitted || surface_->tileWidth() < kMaximumTileWidth);
    syncing_ = false;
}

void WallViewerWindow::toggleFullscreen() {
    // Wayland treats this as a request the compositor answers later, so the
    // controls follow the window state when it changes rather than here.
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void WallViewerWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        const bool fullscreen = isFullScreen();
        fullscreenAction_->setChecked(fullscreen);
        strip_->setVisible(!fullscreen);
    }
}

void WallViewerWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() != Qt::Key_Escape) {
        QWidget::keyPressEvent(event);
        return;
    }
    // Escape backs out one level at a time: out of inspecting a photo, then
    // out of fullscreen, and only then out of the viewer.
    if (auto* tile = qobject_cast<ui::ImageCanvas*>(QApplication::focusWidget());
        tile != nullptr && tile->isInspecting()) {
        tile->setInspecting(false);
    } else if (isFullScreen()) {
        toggleFullscreen();
    } else {
        close();
    }
    event->accept();
}

bool WallViewerWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched != scroll_->viewport() || event->type() != QEvent::Wheel) {
        return QWidget::eventFilter(watched, event);
    }
    auto* wheel = static_cast<QWheelEvent*>(event);
    if (!wheel->modifiers().testFlag(Qt::ControlModifier)) {
        wheelRemainder_ = 0;
        return false; // A plain wheel scrolls.
    }
    wheelRemainder_ += wheel->angleDelta().y();
    const int steps = wheelRemainder_ / kWheelNotch;
    wheelRemainder_ -= steps * kWheelNotch;
    zoomTiles(steps, wheel->position().toPoint());
    return true;
}

} // namespace cullfinch::viewer
