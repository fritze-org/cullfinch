// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/cropper/CropWindow.h>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMetaObject>
#include <QVBoxLayout>

#include <utility>

namespace cullfinch::cropper {

CropWindow::CropWindow(application::IImageService& images, CropPlan plan, CropSaver saver,
                       QWidget* parent)
    : QWidget(parent), plan_(std::move(plan)), saver_(std::move(saver)),
      canvas_(new CropCanvas(images, this)), ahead_(new ui::PreviewLoader(images, this)),
      position_(new QLabel(this)), aspect_(new QLabel(this)), summary_(new QLabel(this)),
      message_(new QLabel(this)) {
    setObjectName(QStringLiteral("cropWindow"));
    setFocusPolicy(Qt::StrongFocus);

    // One at a time: each write holds a full-resolution photo in memory, and
    // in order, so the last error shown belongs to the last photo that failed.
    writer_.setMaxThreadCount(1);

    position_->setObjectName(QStringLiteral("cropPosition"));
    aspect_->setObjectName(QStringLiteral("cropAspect"));
    summary_->setObjectName(QStringLiteral("cropSummary"));
    message_->setObjectName(QStringLiteral("cropMessage"));
    message_->setWordWrap(true);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* help = new QLabel(tr("Drag to crop · Enter save and next · Space skip · ← back · "
                               "Esc reset · 1–6 aspect · R turn · Q quit"),
                            this);
    help->setObjectName(QStringLiteral("cropHelp"));

    auto* status = new QHBoxLayout;
    status->addWidget(position_, 1);
    status->addWidget(aspect_);
    status->addWidget(summary_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 0, 8, 6);
    layout->addWidget(canvas_, 1);
    layout->addWidget(message_);
    layout->addLayout(status);
    layout->addWidget(help);

    connect(canvas_->preview(), &ui::PreviewLoader::readinessChanged, this, [this](bool ready) {
        if (ready) {
            prefetchNext();
            updateStatus();
        }
    });

    if (plan_.targets.isEmpty()) {
        message_->setText(tr("Nothing to crop."));
    } else {
        showPhoto(0);
    }
    updateStatus();
}

CropWindow::~CropWindow() {
    // A write still running is finished rather than abandoned half-way; its
    // result has nowhere to go, which is fine -- the file is what matters.
    writer_.waitForDone();
}

void CropWindow::acceptCrop() {
    if (reachedEnd_ || plan_.targets.isEmpty() || !canvas_->isReady()) {
        return; // Nobody can have meant a crop of a photo they cannot see.
    }
    const QRectF rect = canvas_->cropRect();
    if (isWholeImage(rect)) {
        skip();
        return;
    }

    const CropTarget& target = plan_.targets.at(index_);
    const CropJob job{target.jpegPath, target.rawPaths, rect};
    accepted_.insert(index_, rect);
    ++pending_;
    const int index = index_;
    writer_.start([this, job, index, saver = saver_] {
        const QString error = saver(job);
        // Queued to this window: if it is gone by then, so is the event.
        QMetaObject::invokeMethod(
            this, [this, index, error] { onSaved(index, error); }, Qt::QueuedConnection);
    });
    skip();
}

void CropWindow::skip() {
    if (reachedEnd_ || plan_.targets.isEmpty()) {
        return;
    }
    if (index_ + 1 < photoCount()) {
        showPhoto(index_ + 1);
        return;
    }
    reachedEnd_ = true;
    updateStatus();
    finishIfDone();
}

void CropWindow::back() {
    if (reachedEnd_) {
        // Still on the last photo: stepping back from the end means returning
        // to it, not passing over it.
        reachedEnd_ = false;
        showPhoto(index_);
        return;
    }
    if (index_ > 0) {
        showPhoto(index_ - 1);
    }
}

void CropWindow::waitForSaves() {
    writer_.waitForDone();
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
}

void CropWindow::keyPressEvent(QKeyEvent* event) {
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        acceptCrop();
        break;
    case Qt::Key_Space:
    case Qt::Key_Right:
        skip();
        break;
    case Qt::Key_Left:
        back();
        break;
    case Qt::Key_Escape:
        canvas_->resetCrop();
        break;
    case Qt::Key_R: {
        AspectChoice choice = canvas_->aspect();
        choice.swapped = !choice.swapped;
        canvas_->setAspect(choice);
        updateStatus();
        break;
    }
    case Qt::Key_Q:
        close();
        break;
    default:
        if (const std::optional<AspectPreset> preset = presetForDigit(event->key() - Qt::Key_0)) {
            // A new shape starts the right way round for the photo again.
            canvas_->setAspect(AspectChoice{*preset, false});
            updateStatus();
            break;
        }
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void CropWindow::closeEvent(QCloseEvent* event) {
    // Closing early is fine -- the next run resumes -- but a crop already
    // accepted is written before the window goes.
    if (pending_ > 0) {
        message_->setText(tr("Writing %n crop(s)…", nullptr, pending_));
        message_->repaint();
    }
    waitForSaves();
    event->accept();
}

void CropWindow::showPhoto(int index) {
    index_ = index;
    ++generation_;
    const CropTarget& target = plan_.targets.at(index_);
    canvas_->setPhoto(target.presentation, generation_,
                      accepted_.contains(index_) ? std::optional<QRectF>(accepted_.value(index_))
                                                 : std::nullopt);
    updateStatus();
}

void CropWindow::onSaved(int index, const QString& error) {
    --pending_;
    if (!error.isEmpty()) {
        failures_.append(error);
        // Not cropped after all: going back starts over rather than showing a
        // crop that does not exist on disk.
        accepted_.remove(index);
        message_->setText(error);
    }
    Q_EMIT saveFinished(index, error);
    updateStatus();
    finishIfDone();
}

void CropWindow::finishIfDone() {
    if (!reachedEnd_ || pending_ > 0) {
        return;
    }
    if (!failures_.isEmpty()) {
        message_->setText(tr("Done, but %n crop(s) could not be written:\n%1", nullptr,
                             static_cast<int>(failures_.size()))
                              .arg(failures_.join(QLatin1Char('\n'))));
        return;
    }
    Q_EMIT sequenceFinished();
    close();
}

void CropWindow::prefetchNext() {
    if (index_ + 1 >= photoCount()) {
        return;
    }
    // The same size the canvas will ask for, which is what the cache keys on.
    ahead_->setSource(plan_.targets.at(index_ + 1).presentation, generation_);
    ahead_->requestFitted(canvas_->targetPixels());
}

void CropWindow::updateStatus() {
    if (plan_.targets.isEmpty()) {
        position_->clear();
    } else {
        const CropTarget& target = plan_.targets.at(index_);
        QString text = tr("%1 of %2 · %3")
                           .arg(index_ + 1)
                           .arg(photoCount())
                           .arg(target.presentation.displayName);
        if (target.presentation.rawCount > 0) {
            text += tr(" (+ %n RAW)", nullptr, target.presentation.rawCount);
        }
        position_->setText(text);
    }
    aspect_->setText(aspectLabel(canvas_->aspect(), canvas_->imageSize()));

    QStringList notes;
    if (plan_.alreadyCropped > 0) {
        notes.append(tr("%n already cropped", nullptr, plan_.alreadyCropped));
    }
    if (plan_.withoutPreview > 0) {
        notes.append(tr("%n RAW-only skipped", nullptr, plan_.withoutPreview));
    }
    if (pending_ > 0) {
        notes.append(tr("writing %n", nullptr, pending_));
    }
    summary_->setText(notes.join(QStringLiteral(" · ")));

    if (reachedEnd_ && pending_ > 0) {
        message_->setText(tr("Writing the last crops…"));
    }
}

} // namespace cullfinch::cropper
