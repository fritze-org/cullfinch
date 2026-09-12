// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/QtImageService.h>

#include <QColorSpace>
#include <QCoreApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThread>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <tuple>

namespace cullfinch::infrastructure {
namespace {

/// Bumped whenever the colour policy changes, so cached images from an older
/// policy are never reused.
constexpr int kColourPolicyVersion = 1;

qint64 imageBytes(const QImage& image) {
    return static_cast<qint64>(image.sizeInBytes());
}

QString requestClassToken(application::ImageRequestClass kind) {
    switch (kind) {
    case application::ImageRequestClass::Thumbnail:
        return QStringLiteral("thumb");
    case application::ImageRequestClass::Comparison:
        return QStringLiteral("compare");
    case application::ImageRequestClass::FullResolution:
        break;
    }
    return QStringLiteral("full");
}

} // namespace

QtImageService::QtImageService(QObject* parent) : application::IImageService(parent) {
    // Bounded decoder concurrency: four workers initially.
    pool_.setMaxThreadCount(std::min(4, std::max(1, QThread::idealThreadCount() - 1)));
    pool_.setObjectName(QStringLiteral("cullfinch-decode"));
    // QImageReader's allocation limit is process-global state. It is set
    // here, on the owning thread, before any worker exists; the workers only
    // read it, which is the one access pattern the API is safe for.
    QImageReader::setAllocationLimit(allocationLimitMegabytes_);
}

QtImageService::~QtImageService() {
    pool_.clear();
    pool_.waitForDone();
}

QString QtImageService::cacheKey(const application::ImageRequest& request) {
    return QStringLiteral("%1|%2|%3|%4x%5|%6|c%7")
        .arg(request.memberId.toString())
        .arg(request.fingerprint.sizeBytes)
        .arg(request.fingerprint.modifiedMsecsUtc)
        .arg(request.targetSize.width())
        .arg(request.targetSize.height())
        .arg(requestClassToken(request.kind))
        .arg(kColourPolicyVersion);
}

void QtImageService::setMemoryBudgetBytes(qint64 bytes) {
    QMutexLocker locker(&mutex_);
    budgetBytes_ = std::max<qint64>(bytes, 16LL * 1024 * 1024);
}

qint64 QtImageService::memoryBudgetBytes() const {
    QMutexLocker locker(&mutex_);
    return budgetBytes_;
}

qint64 QtImageService::memoryUsedBytes() const {
    QMutexLocker locker(&mutex_);
    // In-flight decode estimates count towards the budget: a cache size alone
    // would not bound process memory.
    return cacheBytes_ + inFlightBytes_;
}

void QtImageService::setMaximumWorkers(int workers) {
    pool_.setMaxThreadCount(std::max(1, workers));
}

void QtImageService::setAllocationLimitMegabytes(int megabytes) {
    QMutexLocker locker(&mutex_);
    allocationLimitMegabytes_ = std::max(1, megabytes);
    // Applied once, from the owning thread, rather than by every decode
    // worker racing to store the same global.
    QImageReader::setAllocationLimit(allocationLimitMegabytes_);
}

void QtImageService::cancel(quint64 requestId) {
    QMutexLocker locker(&mutex_);
    cancelledRequests_.insert(requestId);
}

void QtImageService::cancelGeneration(quint64 generation) {
    QMutexLocker locker(&mutex_);
    cancelledGenerations_.insert(generation);
}

bool QtImageService::isCancelled(quint64 requestId, quint64 generation) const {
    QMutexLocker locker(&mutex_);
    return cancelledRequests_.contains(requestId) || cancelledGenerations_.contains(generation);
}

quint64 QtImageService::request(const application::ImageRequest& request) {
    const quint64 requestId = nextRequestId_.fetchAndAddOrdered(1);
    const QString key = cacheKey(request);

    {
        QMutexLocker locker(&mutex_);
        const auto cached = cache_.constFind(key);
        if (cached != cache_.constEnd()) {
            // A real copy, not a reference: the mutex unlocks right below and
            // cache_ is mutable from other threads after that, so a reference
            // into it would dangle for the rest of this function.
            const QImage image = *cached; // NOLINT(performance-unnecessary-copy-initialization)
            cacheOrder_.removeAll(key);
            cacheOrder_.append(key);
            locker.unlock();

            application::ImageResult result;
            result.requestId = requestId;
            result.memberId = request.memberId;
            result.generation = request.generation;
            result.kind = request.kind;
            result.image = image;
            result.nativeSize = image.size();
            result.success = true;
            // Delivered asynchronously even on a cache hit, so callers always
            // see the same ordering.
            QMetaObject::invokeMethod(
                this, [this, result]() { Q_EMIT imageReady(result); }, Qt::QueuedConnection);
            return requestId;
        }
    }

    std::ignore = QtConcurrent::run(&pool_, [this, request, requestId, key]() {
        application::ImageResult result;
        result.requestId = requestId;
        result.memberId = request.memberId;
        result.generation = request.generation;
        result.kind = request.kind;

        if (isCancelled(requestId, request.generation)) {
            return;
        }

        QImageReader reader(request.path);
        reader.setAutoTransform(true); // Honour the embedded orientation.

        const QSize native = reader.size();
        if (!native.isValid()) {
            result.error = tr("'%1' could not be read: %2").arg(request.path, reader.errorString());
            deliver(result);
            return;
        }
        result.nativeSize = native;

        if (request.targetSize.isValid() && !request.targetSize.isEmpty()) {
            // Scaled reading: a fit-all wall needs layout items, not
            // full-resolution pixels for a tiny tile.
            QSize scaled = native;
            scaled.scale(request.targetSize, Qt::KeepAspectRatio);
            if (scaled.width() < native.width()) {
                reader.setScaledSize(scaled);
            }
        }

        {
            QMutexLocker locker(&mutex_);
            inFlightBytes_ += static_cast<qint64>(native.width()) * native.height() * 4;
        }

        QImage image = reader.read();

        {
            QMutexLocker locker(&mutex_);
            inFlightBytes_ -= static_cast<qint64>(native.width()) * native.height() * 4;
            if (inFlightBytes_ < 0) {
                inFlightBytes_ = 0;
            }
        }

        if (image.isNull()) {
            result.error =
                tr("'%1' could not be decoded: %2").arg(request.path, reader.errorString());
            deliver(result);
            return;
        }

        // Convert to a defined sRGB working representation. An untagged file is
        // provisionally treated as sRGB and that assumption is recorded.
        if (!image.colorSpace().isValid()) {
            result.colourAssumedSrgb = true;
            image.setColorSpace(QColorSpace::SRgb);
        } else if (image.colorSpace() != QColorSpace(QColorSpace::SRgb)) {
            image.convertToColorSpace(QColorSpace::SRgb);
        }

        result.image = image;
        result.success = true;

        {
            QMutexLocker locker(&mutex_);
            const qint64 bytes = imageBytes(image);
            if (bytes <= budgetBytes_) {
                // Byte-budgeted least-recently-used eviction.
                while (cacheBytes_ + bytes > budgetBytes_ && !cacheOrder_.isEmpty()) {
                    const QString oldest = cacheOrder_.takeFirst();
                    cacheBytes_ -= imageBytes(cache_.value(oldest));
                    cache_.remove(oldest);
                    if (cacheBytes_ < 0) {
                        cacheBytes_ = 0;
                    }
                }
                cache_.insert(key, image);
                cacheOrder_.append(key);
                cacheBytes_ += bytes;
            }
        }

        deliver(result);
    });

    return requestId;
}

void QtImageService::deliver(const application::ImageResult& result) {
    if (isCancelled(result.requestId, result.generation)) {
        return; // A superseded request never reaches the view.
    }
    {
        QMutexLocker locker(&mutex_);
        if (holdResults_) {
            heldResults_.append(result);
            return;
        }
    }
    QMetaObject::invokeMethod(
        this,
        [this, result]() {
            if (isCancelled(result.requestId, result.generation)) {
                return;
            }
            Q_EMIT imageReady(result);
        },
        Qt::QueuedConnection);
}

void QtImageService::holdResultsForTesting() {
    QMutexLocker locker(&mutex_);
    holdResults_ = true;
}

void QtImageService::releaseHeldResultsForTesting() {
    QList<application::ImageResult> ready;
    {
        QMutexLocker locker(&mutex_);
        holdResults_ = false;
        ready.swap(heldResults_);
    }
    // Delivered through the normal path, which re-checks cancellation: a
    // generation abandoned while held must still never reach the view.
    for (const application::ImageResult& result : ready) {
        deliver(result);
    }
}

} // namespace cullfinch::infrastructure
