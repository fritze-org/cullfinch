// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/ui/AssetPresentation.h>

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

namespace cullfinch::ui {

/// The decode requests behind one displayed photo, and what came back.
///
/// Split out of `ImageCanvas` so that the bookkeeping a canvas needs -- which
/// request is current, which result belongs to the photo on screen, whether a
/// preview has arrived -- is not entangled with painting and input handling.
/// See `docs/decisions/0010-preview-loader.md`.
///
/// Readiness is load-bearing rather than cosmetic: decision input stays
/// disabled until every required preview has arrived, so a host that is told
/// "ready" too early lets somebody judge a photo they cannot see.
///
/// Deliberately not a widget. It is told how many pixels to ask for; how that
/// number follows from a widget's size and its device pixel ratio is the
/// canvas's business.
class PreviewLoader : public QObject {
    Q_OBJECT

public:
    explicit PreviewLoader(application::IImageService& images, QObject* parent = nullptr);

    /// Points the loader at another photo. Every result already in flight for
    /// the previous one is abandoned rather than displayed, and readiness
    /// drops to false because nothing has arrived for the new photo yet.
    void setSource(const AssetPresentation& presentation, quint64 generation);

    /// Asks for a screen-sized preview, `targetPixels` device pixels across.
    /// Working that number out is the caller's job: only a widget knows its
    /// buffer size and the scale factor of the output it is on.
    void requestFitted(const QSize& targetPixels);
    /// Asks for the native-resolution image, which is what judging sharpness
    /// needs.
    void requestFullResolution();

    /// Gives the decoded pixels back, keeping the source.
    ///
    /// For a host showing far more photos than fit on screen: a tile scrolled
    /// a long way out of sight would otherwise hold its image for the rest of
    /// the session, so a wall that pre-loads a whole directory would grow
    /// without bound. The decode itself is not lost -- the image service
    /// caches it -- so coming back asks for pixels that are already there.
    ///
    /// Requests in flight are disowned rather than cancelled, for the reason
    /// `setSource` disowns them and for one more: a decode already under way
    /// still lands in the service's cache, which is precisely where a released
    /// tile wants it.
    ///
    /// Readiness drops, so a host that gates a decision on it must not call
    /// this for a photo somebody could be deciding about.
    void release();

    /// Clears a reported error and asks again.
    ///
    /// @return false when there was no error to retry, so a host can leave the
    /// key it came from unhandled.
    bool retryFitted(const QSize& targetPixels);

    /// True once a preview at the requested size has arrived.
    [[nodiscard]] bool isReady() const { return ready_; }
    [[nodiscard]] bool hasError() const { return !errorText_.isEmpty(); }
    [[nodiscard]] QString errorText() const { return errorText_; }
    /// True once pixels at 100% are available.
    [[nodiscard]] bool isFullResolutionReady() const { return fullResolutionReady_; }

    /// True when there is a file to decode at all. An asset whose preview path
    /// is empty is nothing to ask the image service about.
    [[nodiscard]] bool hasSource() const { return !path_.isEmpty(); }
    /// The collection or session generation the current source belongs to.
    [[nodiscard]] quint64 generation() const { return generation_; }

    [[nodiscard]] const QImage& fitted() const { return fitted_; }
    [[nodiscard]] const QImage& full() const { return full_; }
    /// The decoded image's own size, which stays known across a resize even
    /// while the refined preview is still in flight.
    [[nodiscard]] QSize nativeSize() const { return nativeSize_; }

signals:
    /// A preview at the requested size arrived, or was lost to a new source or
    /// a decode error.
    void readinessChanged(bool ready);
    /// The pixels or the error text changed: whatever draws them is stale.
    void changed();
    void retryRequested();

private:
    /// A request naming the current source, for a caller to size and prioritise.
    [[nodiscard]] application::ImageRequest requestFor(application::ImageRequestClass kind) const;
    void onImageReady(const application::ImageResult& result);

    application::IImageService& images_;
    domain::MemberId memberId_;
    QString path_;
    domain::FileFingerprint fingerprint_;
    quint64 generation_ = 0;

    quint64 fitRequestId_ = 0;
    quint64 fullRequestId_ = 0;

    QImage fitted_;
    QImage full_;
    QSize nativeSize_;
    bool ready_ = false;
    bool fullResolutionReady_ = false;
    QString errorText_;
};

} // namespace cullfinch::ui
