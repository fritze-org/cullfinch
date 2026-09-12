// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/views/versus/VersusView.h>

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QShortcut>
#include <QVBoxLayout>

namespace cullfinch::views::versus {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

} // namespace

VersusView::VersusView(application::IImageService& images) : root_(new QWidget) {
    root_->setObjectName(QStringLiteral("versusView"));
    root_->setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(root_);
    layout->setContentsMargins(8, 8, 8, 8);

    matchLabel_ = new QLabel(root_);
    matchLabel_->setObjectName(QStringLiteral("versusMatchLabel"));
    matchLabel_->setAlignment(Qt::AlignCenter);
    layout->addWidget(matchLabel_);

    auto* panes = new QHBoxLayout;
    panes->setSpacing(8);

    left_ = new ui::ImageCanvas(images, root_);
    left_->setObjectName(QStringLiteral("versusLeft"));
    right_ = new ui::ImageCanvas(images, root_);
    right_->setObjectName(QStringLiteral("versusRight"));
    // Equal available area for both panes.
    panes->addWidget(left_, 1);
    panes->addWidget(right_, 1);
    layout->addLayout(panes, 1);

    survivorLabel_ = new QLabel(root_);
    survivorLabel_->setObjectName(QStringLiteral("versusSurvivorLabel"));
    survivorLabel_->setAlignment(Qt::AlignCenter);
    survivorLabel_->setVisible(false);
    layout->addWidget(survivorLabel_);

    keepLeft_ = new QPushButton(tr("Keep left (←)"), root_);
    keepLeft_->setObjectName(QStringLiteral("versusKeepLeft"));
    keepRight_ = new QPushButton(tr("Keep right (→)"), root_);
    keepRight_->setObjectName(QStringLiteral("versusKeepRight"));

    linkViews_ = new QCheckBox(tr("Link zoom and pan"), root_);
    linkViews_->setObjectName(QStringLiteral("versusLinkViews"));
    linkViews_->setToolTip(
        tr("Linked views use normalised image coordinates. When the two photos differ in "
           "resolution or aspect ratio, equal zoom and equal framing are not the same thing: "
           "linked panes match framing, not pixel scale."));

    QObject::connect(left_, &ui::ImageCanvas::eliminateRequested, root_,
                     [this]() { eliminate(leftId_); });
    QObject::connect(right_, &ui::ImageCanvas::eliminateRequested, root_,
                     [this]() { eliminate(rightId_); });
    QObject::connect(keepLeft_, &QPushButton::clicked, root_, [this]() { eliminate(rightId_); });
    QObject::connect(keepRight_, &QPushButton::clicked, root_, [this]() { eliminate(leftId_); });

    QObject::connect(left_, &ui::ImageCanvas::readinessChanged, root_,
                     [this](bool) { updateDecisionAvailability(); });
    QObject::connect(right_, &ui::ImageCanvas::readinessChanged, root_,
                     [this](bool) { updateDecisionAvailability(); });

    QObject::connect(
        left_, &ui::ImageCanvas::viewChanged, root_,
        [this](const QPointF& centre, qreal zoom) { applyLinkedView(centre, zoom, left_); });
    QObject::connect(
        right_, &ui::ImageCanvas::viewChanged, root_,
        [this](const QPointF& centre, qreal zoom) { applyLinkedView(centre, zoom, right_); });

    const auto* keepLeftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), root_);
    QObject::connect(keepLeftShortcut, &QShortcut::activated, root_,
                     [this]() { eliminate(rightId_); });
    const auto* keepRightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), root_);
    QObject::connect(keepRightShortcut, &QShortcut::activated, root_,
                     [this]() { eliminate(leftId_); });
}

QList<QWidget*> VersusView::auxiliaryControls() {
    return {keepLeft_, keepRight_, linkViews_};
}

void VersusView::setPresentations(const ui::AssetPresentationMap& presentations) {
    presentations_ = presentations;
    // Drop the "already showing this candidate" guard: presentations that
    // arrive after a state update must still reach the panes.
    leftId_ = domain::AssetId();
    rightId_ = domain::AssetId();
}

void VersusView::applyLinkedView(const QPointF& centre, qreal zoom, const ui::ImageCanvas* source) {
    if (!linkViews_->isChecked() || applyingLinkedView_) {
        return;
    }
    applyingLinkedView_ = true;
    ui::ImageCanvas* other = (source == left_) ? right_ : left_;
    // Normalised image coordinates, so the same part of each photo is shown
    // even when the two differ in resolution.
    other->setNormalisedView(centre, zoom);
    applyingLinkedView_ = false;
}

void VersusView::setState(const domain::FlowState& state, const domain::FlowSummary& summary) {
    revision_ = state.revision;
    complete_ = summary.complete;

    const flows::versus::MatchView match = flows::versus::VersusFlow::pendingMatch(state);
    matchNode_ = match.node;

    if (!match.isValid()) {
        leftId_ = domain::AssetId();
        rightId_ = domain::AssetId();
        left_->clearPresentation();
        right_->clearPresentation();

        const domain::AssetId winner = flows::versus::VersusFlow::survivor(state);
        survivorLabel_->setVisible(true);
        survivorLabel_->setText(tr("Survivor: %1 · %2 eliminated")
                                    .arg(presentations_.value(winner).displayName)
                                    .arg(summary.draftRejected.size()));
        matchLabel_->setText(tr("Comparison complete. Finish to apply the eliminations."));
        // Finish and Undo remain available on the completed screen.
        updateDecisionAvailability();
        return;
    }

    survivorLabel_->setVisible(false);
    matchLabel_->setText(tr("Round %1, match %2 — click the photo to eliminate it")
                             .arg(match.round)
                             .arg(match.positionInRound + 1));

    if (!(leftId_ == match.left)) {
        leftId_ = match.left;
        left_->setPresentation(presentations_.value(match.left), revision_);
    }
    if (!(rightId_ == match.right)) {
        rightId_ = match.right;
        right_->setPresentation(presentations_.value(match.right), revision_);
    }

    const ui::AssetPresentation leftAsset = presentations_.value(match.left);
    const ui::AssetPresentation rightAsset = presentations_.value(match.right);
    left_->setCaption(tr("%1 · %2").arg(leftAsset.displayName, leftAsset.pairingText));
    right_->setCaption(tr("%1 · %2").arg(rightAsset.displayName, rightAsset.pairingText));

    updateDecisionAvailability();
}

void VersusView::updateDecisionAvailability() {
    // Decision input stays disabled until both required previews are ready, so
    // nobody judges a pair they cannot yet see.
    const bool ready = !complete_ && matchNode_ >= 0 && left_->isReady() && right_->isReady();
    keepLeft_->setEnabled(ready);
    keepRight_->setEnabled(ready);

    if (!complete_ && matchNode_ >= 0 && !ready) {
        matchLabel_->setText(tr("Loading both photos…"));
    }
}

void VersusView::eliminate(const domain::AssetId& id) const {
    if (!sink_ || !id.isValid() || complete_ || matchNode_ < 0) {
        return;
    }
    if (!left_->isReady() || !right_->isReady()) {
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("assetId"), id.toString());
    // The revision the view was rendering: a late click against an already
    // decided match is refused rather than applied to the next one.
    sink_(QString::fromLatin1(flows::versus::kActionEliminate), payload, revision_);
}

void VersusView::setFullscreenPresentation(bool fullscreen) {
    root_->layout()->setContentsMargins(fullscreen ? 0 : 8, fullscreen ? 0 : 8, fullscreen ? 0 : 8,
                                        fullscreen ? 0 : 8);
    matchLabel_->setVisible(!fullscreen);
}

} // namespace cullfinch::views::versus
