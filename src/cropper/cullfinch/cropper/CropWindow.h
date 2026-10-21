// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/cropper/CropCanvas.h>
#include <cullfinch/cropper/CropSequence.h>
#include <cullfinch/cropper/CropWriter.h>
#include <cullfinch/ui/PreviewLoader.h>

#include <QHash>
#include <QLabel>
#include <QRectF>
#include <QStringList>
#include <QThreadPool>
#include <QWidget>

#include <functional>

namespace cullfinch::cropper {

/// Writes one accepted crop, on a worker thread, and returns its errors. A
/// parameter so a test can see what was asked for without encoding JPEGs.
using CropSaver = std::function<QString(const CropJob&)>;

/// One photo after another, each to crop or to skip.
///
/// Accepting a crop moves on at once and writes it in the background: writing
/// means decoding the full-resolution file and encoding it again, which takes
/// long enough per photo that waiting for it would make a long sequence drag.
/// Writes run one at a time, in order, so their memory stays that of one
/// photo. A write that fails is reported in the window and keeps it open at
/// the end, so nobody closes it believing a crop exists that does not.
class CropWindow : public QWidget {
    Q_OBJECT

public:
    CropWindow(application::IImageService& images, CropPlan plan, CropSaver saver = saveCrop,
               QWidget* parent = nullptr);
    ~CropWindow() override;
    CropWindow(const CropWindow&) = delete;
    CropWindow& operator=(const CropWindow&) = delete;
    CropWindow(CropWindow&&) = delete;
    CropWindow& operator=(CropWindow&&) = delete;

    [[nodiscard]] int currentIndex() const { return index_; }
    [[nodiscard]] int photoCount() const { return static_cast<int>(plan_.targets.size()); }
    [[nodiscard]] CropCanvas* canvas() const { return canvas_; }
    [[nodiscard]] int pendingSaves() const { return pending_; }
    [[nodiscard]] const QStringList& failures() const { return failures_; }

    /// Save the crop on screen and move on. A crop that still covers the
    /// whole photo is a skip: writing it would only re-encode the original.
    void acceptCrop();
    /// Move on without writing anything.
    void skip();
    /// Go back one photo, showing the crop accepted for it in this run.
    void back();

    /// Block until every queued write has finished.
    void waitForSaves();

signals:
    /// A queued write finished; `error` is empty on success.
    void saveFinished(int index, const QString& error);
    /// The last photo was passed and every write succeeded. The window closes.
    void sequenceFinished();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void showPhoto(int index);
    void onSaved(int index, const QString& error);
    /// Close once the end has been reached and nothing is left to write.
    void finishIfDone();
    void prefetchNext();
    void updateStatus();

    CropPlan plan_;
    CropSaver saver_;
    CropCanvas* canvas_ = nullptr;
    /// Decodes the next photo while this one is being cropped, so stepping on
    /// finds its preview already in the image service's cache.
    ui::PreviewLoader* ahead_ = nullptr;

    QLabel* position_ = nullptr;
    QLabel* aspect_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* message_ = nullptr;

    QThreadPool writer_;
    int index_ = 0;
    quint64 generation_ = 0;
    int pending_ = 0;
    bool reachedEnd_ = false;
    /// Crops accepted in this run, by step, to show again on the way back.
    QHash<int, QRectF> accepted_;
    QStringList failures_;
};

} // namespace cullfinch::cropper
