// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/PreviewLoader.h>

namespace cullfinch::ui {
namespace {

/// Higher runs first. A photo somebody is looking at now outranks the
/// native-resolution refinement of one they may never zoom into.
constexpr int kComparisonPriority = 100;
constexpr int kFullResolutionPriority = 50;

} // namespace

PreviewLoader::PreviewLoader(application::IImageService& images, QObject* parent)
    : QObject(parent), images_(images) {
    connect(&images_, &application::IImageService::imageReady, this, &PreviewLoader::onImageReady);
}

void PreviewLoader::setSource(const AssetPresentation& presentation, quint64 generation) {
    memberId_ = presentation.previewMemberId;
    path_ = presentation.previewPath;
    fingerprint_ = presentation.previewFingerprint;
    generation_ = generation;

    // Requests already in flight are not cancelled, they are disowned: results
    // are matched against the member id and generation recorded here, so an
    // earlier photo's decode can never be shown as this one.
    fitted_ = QImage();
    full_ = QImage();
    nativeSize_ = QSize();
    ready_ = false;
    fullResolutionReady_ = false;
    errorText_.clear();

    Q_EMIT readinessChanged(false);
    Q_EMIT changed();
}

application::ImageRequest PreviewLoader::requestFor(application::ImageRequestClass kind) const {
    // The identity every answer is matched against: a refinement that arrives
    // after the source changed must be recognisable as belonging to the photo
    // that has gone, which is why the member id and the generation travel with
    // the request rather than being remembered alongside it.
    application::ImageRequest request;
    request.memberId = memberId_;
    request.path = path_;
    request.fingerprint = fingerprint_;
    request.kind = kind;
    request.generation = generation_;
    return request;
}

void PreviewLoader::requestFitted(const QSize& targetPixels) {
    application::ImageRequest request = requestFor(application::ImageRequestClass::Comparison);
    request.targetSize = targetPixels;
    request.priority = kComparisonPriority;
    fitRequestId_ = images_.request(request);
}

void PreviewLoader::requestFullResolution() {
    application::ImageRequest request = requestFor(application::ImageRequestClass::FullResolution);
    // An unset size means native resolution: the decoder scales only for a size
    // that is valid and not empty.
    request.targetSize = QSize();
    request.priority = kFullResolutionPriority;
    fullRequestId_ = images_.request(request);
}

bool PreviewLoader::retryFitted(const QSize& targetPixels) {
    if (errorText_.isEmpty()) {
        return false;
    }
    errorText_.clear();
    requestFitted(targetPixels);
    // The failure message is gone before the replacement arrives, so whatever
    // was drawing it has to redraw rather than keep an error on screen that
    // nothing reports any more.
    Q_EMIT changed();
    Q_EMIT retryRequested();
    return true;
}

void PreviewLoader::onImageReady(const application::ImageResult& result) {
    if (result.generation != generation_ || !(result.memberId == memberId_)) {
        return; // A superseded request, or another loader's image.
    }

    // Which of the two requests this answers. A photo re-requested at a new size
    // leaves its earlier request outstanding, and that earlier answer describes
    // a size this loader no longer wants -- matching the identity is not enough.
    const bool fitted = result.requestId == fitRequestId_;
    if (!fitted && result.requestId != fullRequestId_) {
        return;
    }

    if (!result.success) {
        // A decode error is a reported failure, never a rejection decision.
        //
        // Only the fitted preview's failure is the photo failing. The
        // refinement is what inspection zooms into, and a photo whose fitted
        // preview is on screen stays decidable whether or not that arrives:
        // taking readiness away here would disable a decision about pixels the
        // user is looking at.
        if (!fitted) {
            return;
        }
        errorText_ = result.error;
        ready_ = false;
        Q_EMIT readinessChanged(false);
        Q_EMIT changed();
        return;
    }

    nativeSize_ = result.nativeSize;
    if (fitted) {
        fitted_ = result.image;
        errorText_.clear();
        ready_ = true;
        Q_EMIT readinessChanged(true);
    } else {
        full_ = result.image;
        fullResolutionReady_ = true;
    }
    Q_EMIT changed();
}

} // namespace cullfinch::ui
