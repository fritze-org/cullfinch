// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/views/wall/WallView.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonObject>
#include <QResizeEvent>
#include <QStyleHints>
#include <QVBoxLayout>

namespace cullfinch::views::wall {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

} // namespace

WallSurface::WallSurface(application::IImageService& images, QWidget* parent)
    : QWidget(parent), images_(images) {
    setObjectName(QStringLiteral("wallSurface"));
    setFocusPolicy(Qt::StrongFocus);
    sinceStart_.start();
}

void WallSurface::setCandidates(const QList<domain::AssetId>& order,
                                const ui::AssetPresentationMap& presentations, quint64 revision) {
    presentations_ = presentations;

    // Record where departing tiles used to be, so a second click at the same
    // coordinates does not immediately hit whatever moved in.
    for (const domain::AssetId& id : order_) {
        if (!order.contains(id)) {
            ui::ImageCanvas* tile = tiles_.value(id, nullptr);
            if (tile != nullptr) {
                vanished_.append(VanishedTile{tile->geometry(), sinceStart_.elapsed()});
            }
        }
    }

    order_ = order;
    revision_ = revision;

    // Remove tiles that are gone.
    const QList<domain::AssetId> existing = tiles_.keys();
    for (const domain::AssetId& id : existing) {
        if (!order_.contains(id)) {
            tiles_.take(id)->deleteLater();
        }
    }

    // Add tiles that are new.
    for (const domain::AssetId& id : order_) {
        if (tiles_.contains(id)) {
            continue;
        }
        auto* tile = new ui::ImageCanvas(images_, this);
        tile->setObjectName(QStringLiteral("wallTile_") + id.toString());
        tile->setPresentation(presentations_.value(id), revision_);
        tile->setCaption(presentations_.value(id).displayName);
        connect(tile, &ui::ImageCanvas::eliminateRequested, this, [this, id]() {
            if (acceptGesture(id)) {
                Q_EMIT eliminateRequested(id, revision_);
            }
        });
        connect(tile, &ui::ImageCanvas::readinessChanged, this, [this, id](bool ready) {
            if (ready) {
                recordAspect(id);
            }
        });
        tile->show();
        tiles_.insert(id, tile);
    }

    relayout();
}

bool WallSurface::acceptGesture(const domain::AssetId& id) {
    ui::ImageCanvas* tile = tiles_.value(id, nullptr);
    if (tile == nullptr) {
        return false;
    }

    // Suppress a repeat click landing in the region a tile just left, unless
    // the pointer deliberately moved to another target. Finishing a layout
    // animation alone never rearms a second click at the same coordinates.
    const int interval = QGuiApplication::styleHints()->mouseDoubleClickInterval();
    const QPoint pointer = tile->mapTo(this, tile->rect().center());
    for (const VanishedTile& gone : vanished_) {
        if (sinceStart_.elapsed() - gone.elapsedAtRemoval > interval) {
            continue;
        }
        if (gone.region.contains(pointer)) {
            return false;
        }
    }
    return true;
}

void WallSurface::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    relayout();
}

void WallSurface::recordAspect(const domain::AssetId& id) {
    if (aspects_.contains(id)) {
        return; // Recorded once per candidate, so the grid cannot oscillate.
    }
    ui::ImageCanvas* tile = tiles_.value(id, nullptr);
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

    QList<flows::wall::LayoutItem> items;
    items.reserve(order_.size());
    for (const domain::AssetId& id : order_) {
        flows::wall::LayoutItem item;
        item.id = id;
        // A placeholder aspect until the preview decodes, so tiles do not jump
        // the moment an image arrives.
        item.imageSize = aspects_.value(id);
        items.append(item);
    }

    const flows::wall::WallLayoutResult layout =
        flows::wall::WallLayout::compute(QSizeF(size()), items);
    for (const flows::wall::LayoutCell& cell : layout.cells) {
        ui::ImageCanvas* tile = tiles_.value(cell.id, nullptr);
        if (tile != nullptr) {
            tile->setGeometry(cell.cellRect.toRect());
        }
    }
}

ui::ImageCanvas* WallSurface::tileFor(const domain::AssetId& id) const {
    return tiles_.value(id, nullptr);
}

WallView::WallView(application::IImageService& images) {
    root_ = new QWidget;
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

    // Placeholders are represented by absence here: the surface lays out the
    // real candidates, and fixed-position mode simply keeps their indices
    // stable because the flow does not remove the slot.
    const QList<flows::wall::WallSlot> positions = flows::wall::WallFlow::positions(state);
    QList<domain::AssetId> order;
    order.reserve(positions.size());
    for (const flows::wall::WallSlot& slot : positions) {
        if (!slot.isPlaceholder()) {
            order.append(slot.id);
        }
    }
    surface_->setCandidates(order, presentations_, revision_);

    updatingControls_ = true;
    fixedPositions_->setChecked(flows::wall::WallFlow::layoutMode(state) ==
                                flows::wall::LayoutMode::FixedPositions);
    updatingControls_ = false;
    compact_->setEnabled(flows::wall::WallFlow::hasPlaceholders(state));

    // Finishing may retain any number of survivors, including zero, and the
    // empty wall still offers Undo and Finish.
    emptyLabel_->setVisible(summary.remaining.isEmpty());
    surface_->setVisible(!summary.remaining.isEmpty());
}

void WallView::setFullscreenPresentation(bool fullscreen) {
    const int margin = fullscreen ? 0 : 4;
    root_->layout()->setContentsMargins(margin, margin, margin, margin);
}

} // namespace cullfinch::views::wall
