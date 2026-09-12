// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>

#include <QAtomicInteger>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QThreadPool>

namespace cullfinch::infrastructure {

/// Decoding through QImageReader, off the GUI thread.
///
/// QImageReader supplies the automatic orientation transform, scaled reading
/// and decode error reporting; the scaling efficiency depends on the format
/// handler, so the bundled JPEG plugin is what this is tuned against. Display
/// object conversion stays on the GUI thread.
class QtImageService final : public application::IImageService {
    Q_OBJECT

public:
    explicit QtImageService(QObject* parent = nullptr);
    ~QtImageService() override;

    QtImageService(const QtImageService&) = delete;
    QtImageService& operator=(const QtImageService&) = delete;
    QtImageService(QtImageService&&) = delete;
    QtImageService& operator=(QtImageService&&) = delete;

    quint64 request(const application::ImageRequest& request) override;
    void cancel(quint64 requestId) override;
    void cancelGeneration(quint64 generation) override;

    void setMemoryBudgetBytes(qint64 bytes) override;
    [[nodiscard]] qint64 memoryBudgetBytes() const override;
    [[nodiscard]] qint64 memoryUsedBytes() const override;

    /// Upper bound on simultaneously decoding workers, reduced further when the
    /// predicted memory use requires it.
    void setMaximumWorkers(int workers);

    /// Refuse images whose reported dimensions are implausibly large before any
    /// buffer is allocated.
    void setAllocationLimitMegabytes(int megabytes);

    /// Cache key: member identity, fingerprint, requested size, orientation and
    /// colour-policy version. A replaced file can never reuse an old image.
    [[nodiscard]] static QString cacheKey(const application::ImageRequest& request);

    /// Test-only: hold decode results instead of delivering them, so a test
    /// can observe UI state a result would otherwise already have reached. A
    /// result computed while held is queued, never dropped, and delivered
    /// once releaseHeldResultsForTesting() is called.
    void holdResultsForTesting();
    void releaseHeldResultsForTesting();

private:
    void deliver(const application::ImageResult& result);
    [[nodiscard]] bool isCancelled(quint64 requestId, quint64 generation) const;
    /// Decode one request and deliver it. Runs on a pool thread, which is why it
    /// touches nothing but the mutex-guarded members.
    void decodeAndDeliver(const application::ImageRequest& request, quint64 requestId,
                          const QString& key);
    /// Insert under the byte budget, evicting least-recently-used entries first.
    /// An image larger than the whole budget is delivered but never cached.
    void cacheImage(const QString& key, const QImage& image);

    QThreadPool pool_;
    QAtomicInteger<quint64> nextRequestId_ = 1;

    mutable QMutex mutex_;
    QSet<quint64> cancelledRequests_;
    QSet<quint64> cancelledGenerations_;
    QHash<QString, QImage> cache_;
    QList<QString> cacheOrder_; ///< Least-recently-used first.
    qint64 cacheBytes_ = 0;
    qint64 inFlightBytes_ = 0;
    /// Design setting, not a measured optimum.
    qint64 budgetBytes_ = 512LL * 1024 * 1024;
    int allocationLimitMegabytes_ = 512;

    /// Guarded by mutex_. Off in production; a test flips it on to hold
    /// results for a bounded window.
    bool holdResults_ = false;
    QList<application::ImageResult> heldResults_;
};

} // namespace cullfinch::infrastructure
