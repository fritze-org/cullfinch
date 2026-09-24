// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/FakeImageService.h>

namespace cullfinch::testsupport {

quint64 FakeImageService::request(const application::ImageRequest& request) {
    requests_.append(request);
    // Identifiers start at 1: a loader that has asked for nothing holds 0, and
    // that must never match an answer.
    return static_cast<quint64>(requests_.size());
}

void FakeImageService::cancel(quint64 /*requestId*/) {}

void FakeImageService::cancelGeneration(quint64 /*generation*/) {}

void FakeImageService::setMemoryBudgetBytes(qint64 bytes) {
    budget_ = bytes;
}

qint64 FakeImageService::memoryBudgetBytes() const {
    return budget_;
}

void FakeImageService::setMemoryUsedBytes(qint64 bytes) {
    used_ = bytes;
}

qint64 FakeImageService::memoryUsedBytes() const {
    return used_;
}

application::ImageResult FakeImageService::answerFor(qsizetype index) const {
    const application::ImageRequest& asked = requests_.at(index);
    application::ImageResult result;
    result.requestId = static_cast<quint64>(index) + 1;
    result.memberId = asked.memberId;
    result.generation = asked.generation;
    result.kind = asked.kind;
    return result;
}

void FakeImageService::succeed(qsizetype index, const QSize& size) {
    application::ImageResult result = answerFor(index);
    result.success = true;
    result.image = QImage(size, QImage::Format_RGB32);
    result.image.fill(Qt::gray);
    result.nativeSize = size;
    deliver(result);
}

void FakeImageService::fail(qsizetype index, const QString& error) {
    application::ImageResult result = answerFor(index);
    result.success = false;
    result.error = error;
    deliver(result);
}

void FakeImageService::deliver(const application::ImageResult& result) {
    Q_EMIT imageReady(result);
}

} // namespace cullfinch::testsupport
