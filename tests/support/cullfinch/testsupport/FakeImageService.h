// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>

#include <QImage>
#include <QList>
#include <QSize>
#include <QString>

namespace cullfinch::testsupport {

/// An image service that decodes nothing and answers only when told to.
///
/// Decoding is asynchronous in production, so what a loader or a canvas does
/// depends on *when* a result arrives and which request it belongs to. Holding
/// every request until the test answers it is what makes those orderings
/// reachable, and a failure is produced by asking for one rather than by
/// keeping a deliberately broken file around.
class FakeImageService final : public application::IImageService {
public:
    quint64 request(const application::ImageRequest& request) override;
    void cancel(quint64 requestId) override;
    void cancelGeneration(quint64 generation) override;
    void setMemoryBudgetBytes(qint64 bytes) override;
    [[nodiscard]] qint64 memoryBudgetBytes() const override;
    [[nodiscard]] qint64 memoryUsedBytes() const override;

    /// Every request made so far, oldest first. A request's index here is how
    /// the answering helpers name it.
    [[nodiscard]] const QList<application::ImageRequest>& requests() const { return requests_; }

    /// Answers request `index` with an image of `size`, reported as the file's
    /// native size.
    void succeed(int index, const QSize& size = QSize(400, 300));
    /// Answers request `index` with a decode failure.
    void fail(int index, const QString& error);
    /// Delivers a result the caller built, for the cases about results that
    /// have to be ignored.
    void deliver(const application::ImageResult& result);
    /// The envelope of the answer to request `index`: which request it belongs
    /// to, and which photo and which generation it was asked for.
    [[nodiscard]] application::ImageResult answerFor(int index) const;

private:
    QList<application::ImageRequest> requests_;
    qint64 budget_ = 0;
};

} // namespace cullfinch::testsupport
