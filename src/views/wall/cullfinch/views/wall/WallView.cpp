// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/views/wall/WallView.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonObject>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyleHints>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cullfinch::views::wall {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

/// How long a tile on a scrolled wall waits for its size to settle before
/// decoding at the new size. Long enough to span the steps of a slider drag,
/// short enough that the sharper image follows as soon as the drag stops.
constexpr int kRefinementDelayMs = 150;

/// The minimum ImageCanvas sets for itself, restored when a wall goes back to
/// fitting its tiles.
constexpr QSize kFittedTileMinimum(64, 64);

/// True while any of these wall positions refers to this candidate, whether it
/// survives there or is held as an eliminated placeholder.
bool holdsCandidate(const QList<flows::wall::WallSlot>& positions, const domain::AssetId& id) {
    return std::ranges::any_of(positions,
                               [&id](const flows::wall::WallSlot& slot) { return slot.id == id; });
}

} // namespace

WallSurface::WallSurface(application::IImageService& images, QWidget* parent)
    : QWidget(parent), images_(images) {
    setObjectName(QStringLiteral("wallSurface"));
    setFocusPolicy(Qt::StrongFocus);
    sinceStart_.start();
}

QList<domain::AssetId> WallSurface::order() const {
    QList<domain::AssetId> candidates;
    for (const flows::wall::WallSlot& slot : positions_) {
        if (!slot.isPlaceholder()) {
            candidates.append(slot.id);
        }
    }
    return candidates;
}

void WallSurface::recordVanishedTiles(const QList<flows::wall::WallSlot>& positions) {
    for (const flows::wall::WallSlot& slot : positions_) {
        // An eliminated candidate whose cell is being held has not vanished:
        // its tile is still there, marked as eliminated.
        if (!slot.id.isValid() || holdsCandidate(positions, slot.id)) {
            continue;
        }
        if (const ui::ImageCanvas* tile = tiles_.value(slot.id, nullptr); tile != nullptr) {
            vanished_.append(VanishedTile{tile->geometry(), sinceStart_.elapsed()});
        }
    }
}

void WallSurface::removeDepartedTiles() {
    const QList<domain::AssetId> existing = tiles_.keys();
    for (const domain::AssetId& id : existing) {
        if (!holdsCandidate(positions_, id)) {
            tiles_.take(id)->deleteLater();
        }
    }
}

void WallSurface::addTile(const domain::AssetId& id) {
    auto* tile = new ui::ImageCanvas(images_, this);
    tile->setObjectName(QStringLiteral("wallTile_") + id.toString());
    // Held back before the photo is set, so a tile created off screen never
    // asks for anything until it comes near the viewport.
    if (tileWidth_ > 0) {
        tile->setRefinementDelay(kRefinementDelayMs);
        tile->setLoadingDeferred(true);
    }
    tile->setPresentation(presentations_.value(id), revision_);
    tile->setCaption(presentations_.value(id).displayName);
    connect(tile, &ui::ImageCanvas::gestureArmed, this, [this, id, tile](const QPoint& at) {
        if (tile->isRejected()) {
            return; // Already eliminated: not a decision target until Undo.
        }
        armedTile_ = id;
        armedRevision_ = revision_;
        armedPosition_ = tile->mapTo(this, at);
    });
    connect(tile, &ui::ImageCanvas::eliminateRequested, this,
            [this, id, tile](ui::ImageCanvas::ActivationSource source) {
                if (tile->isRejected()) {
                    // Fixed-position mode keeps the tile on the wall so the
                    // user can see what the cell is being held for. Clicking it
                    // again, or pressing Delete on it, decides nothing.
                    return;
                }
                // A pointer gesture is judged against the layout it was pressed
                // on. A key has no press, so it acts on the current layout --
                // and never on whatever a cancelled pointer gesture left behind.
                const bool pointer =
                    source == ui::ImageCanvas::ActivationSource::Pointer && armedTile_ == id;
                const quint64 against = pointer ? armedRevision_ : revision_;
                const QPoint at =
                    pointer ? armedPosition_ : tile->mapTo(this, tile->rect().center());
                armedTile_ = domain::AssetId{};
                if (acceptGesture(id, at)) {
                    Q_EMIT eliminateRequested(id, against);
                }
            });
    connect(tile->preview(), &ui::PreviewLoader::readinessChanged, this, [this, id](bool ready) {
        if (ready) {
            recordAspect(id);
        }
    });
    tile->show();
    tiles_.insert(id, tile);
}

void WallSurface::setCandidates(const QList<flows::wall::WallSlot>& positions,
                                const ui::AssetPresentationMap& presentations, quint64 revision) {
    presentations_ = presentations;
    recordVanishedTiles(positions);

    positions_ = positions;
    revision_ = revision;
    removeDepartedTiles();

    // Add tiles that are new, and fill in any created before its presentation
    // was known.
    for (const flows::wall::WallSlot& slot : positions_) {
        if (!slot.id.isValid()) {
            continue; // A draft from before placeholders kept their candidate.
        }
        if (!tiles_.contains(slot.id)) {
            addTile(slot.id);
        } else {
            ui::ImageCanvas* placed = tiles_.value(slot.id);
            if (const ui::AssetPresentation& known = presentations_[slot.id];
                !placed->presentation().previewMemberId.isValid() &&
                known.previewMemberId.isValid()) {
                placed->setPresentation(known, revision_);
                placed->setCaption(known.displayName);
            }
        }
        // An eliminated candidate keeps its cell in fixed-position mode, shown
        // as eliminated rather than as an anonymous gap: that is what makes the
        // placeholder readable instead of merely reserved. Undo clears the mark
        // again, and Compact takes the tile away.
        tiles_.value(slot.id)->setRejected(slot.rejected);
    }

    relayout();
}

bool WallSurface::acceptGesture(const domain::AssetId& id, const QPoint& pointer) {
    if (!tiles_.contains(id)) {
        return false;
    }

    // Suppress a repeat click landing in the region a tile just left, unless
    // the pointer deliberately moved to another target. Finishing a layout
    // animation alone never rearms a second click at the same coordinates.
    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    if (const bool movedDeliberately =
            lastGesturePosition_.x() >= 0 && (pointer - lastGesturePosition_).manhattanLength() >
                                                 QGuiApplication::styleHints()->startDragDistance();
        !movedDeliberately) {
        for (const VanishedTile& gone : vanished_) {
            if (sinceStart_.elapsed() - gone.elapsedAtRemoval > interval) {
                continue;
            }
            if (gone.region.contains(pointer)) {
                return false;
            }
        }
    }
    lastGesturePosition_ = pointer;
    return true;
}

void WallSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void WallSurface::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    // A scroll area scrolls by moving this surface under its viewport.
    updateDeferredLoading();
}

void WallSurface::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    updateDeferredLoading();
}

void WallSurface::setTileWidth(int pixels) {
    pixels = std::max(0, pixels);
    if (pixels == tileWidth_) {
        return;
    }
    tileWidth_ = pixels;
    for (ui::ImageCanvas* tile : std::as_const(tiles_)) {
        tile->setRefinementDelay(tileWidth_ > 0 ? kRefinementDelayMs : 0);
    }
    relayout();
}

QRect WallSurface::shownArea() const {
    const QWidget* viewport = parentWidget();
    if (viewport == nullptr) {
        return rect();
    }
    return rect().intersected(QRect(mapFrom(viewport, QPoint(0, 0)), viewport->size()));
}

void WallSurface::updateDeferredLoading() {
    if (tileWidth_ <= 0) {
        // A fitted wall shows every tile, so every tile loads.
        for (ui::ImageCanvas* tile : std::as_const(tiles_)) {
            tile->setLoadingDeferred(false);
        }
        return;
    }
    // Hidden, nothing is shown yet: loading waits for the first real viewport
    // rather than guessing one.
    const QRect shown = isVisible() ? shownArea() : QRect();
    if (shown.isEmpty()) {
        return;
    }
    // A screen above and below as well, so scrolling at a normal pace finds
    // the next photos already decoded.
    const QRect ahead = shown.adjusted(0, -shown.height(), 0, shown.height());
    for (ui::ImageCanvas* tile : std::as_const(tiles_)) {
        tile->setLoadingDeferred(!ahead.intersects(tile->geometry()));
    }
}

void WallSurface::recordAspect(const domain::AssetId& id) {
    if (aspects_.contains(id)) {
        return; // Recorded once per candidate, so the grid cannot oscillate.
    }
    const ui::ImageCanvas* tile = tiles_.value(id, nullptr);
    if (tile == nullptr) {
        return;
    }
    const QRectF drawn = tile->imageRect();
    if (drawn.width() <= 0.0 || drawn.height() <= 0.0) {
        return;
    }
    aspects_.insert(id, drawn.size());
    relayout();
}

void WallSurface::relayout() {
    // Expire stale suppression regions so the list cannot grow without bound.
    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    const qint64 now = sinceStart_.elapsed();
    vanished_.removeIf([now, interval](const VanishedTile& gone) {
        return now - gone.elapsedAtRemoval > interval;
    });

    // Placeholders take part in the layout. That is what makes fixed-position
    // mode work: the grid keeps the same shape, so every survivor stays exactly
    // where the user last saw it until they ask to compact.
    QList<flows::wall::LayoutItem> items;
    items.reserve(positions_.size());
    for (const flows::wall::WallSlot& slot : positions_) {
        flows::wall::LayoutItem item;
        item.id = slot.id;
        // A placeholder aspect until the preview decodes, so tiles do not jump
        // the moment an image arrives.
        item.imageSize = aspects_.value(slot.id);
        items.append(item);
    }

    flows::wall::WallLayoutOptions options;
    options.cellWidth = tileWidth_;
    const flows::wall::WallLayoutResult layout =
        flows::wall::WallLayout::compute(QSizeF(size()), items, options);
    for (const flows::wall::LayoutCell& cell : layout.cells) {
        ui::ImageCanvas* tile = tiles_.value(cell.id, nullptr);
        if (tile != nullptr) {
            // A fixed-width cell narrowed to fit a narrow window can be shorter
            // than the canvas's own minimum, and Qt would then grow the tile
            // over the row below. The fitted wall keeps the canvas's minimum.
            tile->setMinimumSize(tileWidth_ > 0 ? QSize(1, 1) : kFittedTileMinimum);
            tile->setGeometry(cell.cellRect.toRect());
        }
    }

    // A grid taller than the surface asks its scroll area for the room. The
    // fitted grid must not keep a height an earlier tile width asked for --
    // but only that height is this surface's to take back: a host that fixed
    // the surface's size, as the comparison wall's hosts may, keeps it.
    const int needed =
        tileWidth_ > 0 ? std::max(0, static_cast<int>(std::ceil(layout.contentSize.height()))) : 0;
    if (needed != requestedHeight_) {
        requestedHeight_ = needed;
        setMinimumHeight(needed);
    }
    updateDeferredLoading();
}

ui::ImageCanvas* WallSurface::tileFor(const domain::AssetId& id) const {
    return tiles_.value(id, nullptr);
}

WallView::WallView(application::IImageService& images) : root_(new QWidget) {
    root_->setObjectName(QStringLiteral("wallView"));

    auto* layout = new QVBoxLayout(root_);
    layout->setContentsMargins(4, 4, 4, 4);

    surface_ = new WallSurface(images, root_);
    layout->addWidget(surface_, 1);

    emptyLabel_ =
        new QLabel(tr("Every photo has been eliminated. Undo, or Finish to apply."), root_);
    emptyLabel_->setObjectName(QStringLiteral("wallEmptyLabel"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    emptyLabel_->setVisible(false);
    layout->addWidget(emptyLabel_);

    fixedPositions_ = new QCheckBox(tr("Keep positions until Compact"), root_);
    fixedPositions_->setObjectName(QStringLiteral("wallFixedPositions"));
    fixedPositions_->setToolTip(
        tr("Leave a placeholder where an eliminated photo was, which preserves spatial memory "
           "during rapid culling."));

    compact_ = new QPushButton(tr("Compact"), root_);
    compact_->setObjectName(QStringLiteral("wallCompact"));
    compact_->setEnabled(false);

    QObject::connect(surface_, &WallSurface::eliminateRequested, root_,
                     [this](const domain::AssetId& id, quint64 layoutRevision) {
                         if (!sink_) {
                             return;
                         }
                         QJsonObject payload;
                         payload.insert(QStringLiteral("assetId"), id.toString());
                         sink_(QString::fromLatin1(flows::wall::kActionEliminate), payload,
                               layoutRevision);
                     });

    QObject::connect(fixedPositions_, &QCheckBox::toggled, root_, [this](bool checked) {
        if (!sink_ || updatingControls_) {
            return;
        }
        QJsonObject payload;
        payload.insert(QStringLiteral("mode"),
                       checked ? QStringLiteral("fixed") : QStringLiteral("reflow"));
        sink_(QString::fromLatin1(flows::wall::kActionSetLayoutMode), payload, revision_);
    });

    QObject::connect(compact_, &QPushButton::clicked, root_, [this]() {
        if (sink_) {
            sink_(QString::fromLatin1(flows::wall::kActionCompact), QJsonObject{}, revision_);
        }
    });
}

QList<QWidget*> WallView::auxiliaryControls() {
    return {fixedPositions_, compact_};
}

void WallView::setPresentations(const ui::AssetPresentationMap& presentations) {
    presentations_ = presentations;
}

void WallView::setState(const domain::FlowState& state, const domain::FlowSummary& summary) {
    revision_ = state.revision;

    // Every wall position is forwarded, placeholders included: dropping them
    // here would shrink the grid on every elimination and move the survivors,
    // which is precisely what fixed-position mode exists to prevent. The
    // surface also needs the placeholder's candidate to draw its tile as
    // eliminated instead of leaving a blank cell.
    const QList<flows::wall::WallSlot> positions = flows::wall::WallFlow::positions(state);
    surface_->setCandidates(positions, presentations_, revision_);

    updatingControls_ = true;
    fixedPositions_->setChecked(flows::wall::WallFlow::layoutMode(state) ==
                                flows::wall::LayoutMode::FixedPositions);
    updatingControls_ = false;
    compact_->setEnabled(flows::wall::WallFlow::hasPlaceholders(state));

    // Finishing may retain any number of survivors, including zero, and the
    // empty wall still offers Undo and Finish.
    //
    // The surface stays up while any cell still names a photo, which is not the
    // same as having a survivor: in fixed-position mode the cells left after
    // the last elimination hold the eliminated photos themselves, and hiding
    // them would take the spatial record away exactly when the wall is
    // emptiest and the user is most likely to want Undo.
    emptyLabel_->setVisible(summary.remaining.isEmpty());
    surface_->setVisible(std::ranges::any_of(
        positions, [](const flows::wall::WallSlot& slot) { return slot.id.isValid(); }));
}

void WallView::setFullscreenPresentation(bool fullscreen) {
    const int margin = fullscreen ? 0 : 4;
    root_->layout()->setContentsMargins(margin, margin, margin, margin);
}

} // namespace cullfinch::views::wall
