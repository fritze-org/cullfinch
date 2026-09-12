// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/FileMember.h>
#include <cullfinch/domain/Ids.h>

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QSize>
#include <QString>

namespace cullfinch::application {

/// Three request classes with different size and priority policies.
enum class ImageRequestClass {
    Thumbnail,     ///< Browser grid; smallest, lowest priority.
    Comparison,    ///< Screen-sized image for the current pair or wall.
    FullResolution ///< 100% inspection, for judging sharpness.
};

/// A decode request.
///
/// The cache key is the member identity, its fingerprint, the requested pixel
/// size, the orientation transform and the colour-policy version, so a replaced
/// file can never reuse an old cached image.
struct ImageRequest {
    domain::MemberId memberId;
    QString path;
    domain::FileFingerprint fingerprint;
    QSize targetSize; ///< Null means native resolution.
    ImageRequestClass kind = ImageRequestClass::Thumbnail;
    int priority = 0; ///< Higher runs first.
    /// Collection or session generation. Results from a superseded generation
    /// are discarded rather than displayed.
    quint64 generation = 0;
};

struct ImageResult {
    quint64 requestId = 0;
    domain::MemberId memberId;
    quint64 generation = 0;
    ImageRequestClass kind = ImageRequestClass::Thumbnail;
    QImage image;
    QSize nativeSize;
    bool success = false;
    QString error;
    /// True when the file carried no colour profile and sRGB was assumed. The
    /// assumption is recorded rather than hidden.
    bool colourAssumedSrgb = false;
};

/// Decoding, orientation, colour conversion and caching.
///
/// A decode error is a reported failure, never a rejection decision.
class IImageService : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    /// @return the request identifier, for cancellation.
    virtual quint64 request(const ImageRequest& request) = 0;
    virtual void cancel(quint64 requestId) = 0;
    /// Abandon every request belonging to a superseded generation.
    virtual void cancelGeneration(quint64 generation) = 0;

    /// Byte budget covering cached images, in-flight decode estimates and
    /// displayed buffers. A cache-size setting alone would not bound process
    /// memory.
    virtual void setMemoryBudgetBytes(qint64 bytes) = 0;
    [[nodiscard]] virtual qint64 memoryBudgetBytes() const = 0;
    [[nodiscard]] virtual qint64 memoryUsedBytes() const = 0;

signals:
    void imageReady(const cullfinch::application::ImageResult& result);
};

} // namespace cullfinch::application

Q_DECLARE_METATYPE(cullfinch::application::ImageResult)
Q_DECLARE_METATYPE(cullfinch::application::ImageRequest)
