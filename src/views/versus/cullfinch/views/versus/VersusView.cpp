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

    QObject::connect(linkViews_, &QCheckBox::toggled, root_, [this](bool linked) {
        if (!linked) {
            return;
        }
        // Linking has to take effect when it is asked for. Waiting for the next
        // pan would leave two differently framed photos while the box says they
        // are linked. The left pane is the reference, so what the panes converge
        // on does not depend on which one happened to be touched last.
        applyLinkedView(left_->normalisedCentre(), left_->zoom(), left_);
    });

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
        const domain::AssetId winner = flows::versus::VersusFlow::survivor(state);
        const ui::AssetPresentation survivor = presentations_.value(winner);

        // Completion shows the survivor, and showing a photo means the photo:
        // the left pane keeps it, marked as the selection the flow is keeping,
        // and the opposing pane goes away instead of sitting there empty. It is
        // the same screen the single-candidate case lands on.
        //
        // The identity guard matters as much here as it does for a match: this
        // runs again whenever the session reports state, and re-presenting the
        // same photo would restart its decode.
        if (!(leftId_ == winner)) {
            leftId_ = winner;
            rightId_ = domain::AssetId();
            right_->clearPresentation();
            if (winner.isValid()) {
                left_->setPresentation(survivor, revision_);
                left_->setCaption(tr("%1 · kept").arg(survivor.displayName));
                left_->setSelectionHighlighted(true);
            } else {
                left_->clearPresentation();
            }
        }
        right_->setVisible(false);

        survivorLabel_->setVisible(true);
        survivorLabel_->setText(tr("Survivor: %1 · %2 eliminated")
                                    .arg(survivor.displayName)
                                    .arg(summary.draftRejected.size()));
        matchLabel_->setText(tr("Comparison complete. Finish to apply the eliminations."));
        // Finish and Undo remain available on the completed screen.
        updateDecisionAvailability();
        return;
    }

    survivorLabel_->setVisible(false);
    // Undo can come back from the completed screen, which is where the second
    // pane and the survivor mark were left behind.
    right_->setVisible(true);
    left_->setSelectionHighlighted(false);
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
