// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/CollectionController.h>
#include <cullfinch/domain/AssociationPolicy.h>

#include <QAtomicInteger>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QList>
#include <QMutex>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace cullfinch::infrastructure {

/// Asynchronous directory discovery and JPG/RAW association.
///
/// Enumeration happens on a worker; results are delivered to the GUI thread by
/// queued invocation and tagged with the scan generation, so results from a
/// superseded directory are discarded rather than displayed.
class DirectoryScanner final : public application::IScanService {
    Q_OBJECT

public:
    explicit DirectoryScanner(QObject* parent = nullptr);
    ~DirectoryScanner() override;

    DirectoryScanner(const DirectoryScanner&) = delete;
    DirectoryScanner& operator=(const DirectoryScanner&) = delete;
    DirectoryScanner(DirectoryScanner&&) = delete;
    DirectoryScanner& operator=(DirectoryScanner&&) = delete;

    void requestScan(const application::ScanRequest& request) override;
    void cancelAll() override;
    void setWatchEnabled(bool enabled) override;

    /// Enumerate a directory synchronously. Exposed for tests and reused by the
    /// worker; performs no signalling and touches no Qt object affinity.
    [[nodiscard]] static QList<domain::DiscoveredFile> enumerate(const QString& rootPath,
                                                                 bool recursive, QString* error);

private:
    void publishFinished(quint64 generation, const domain::AssociationResult& result);
    void publishFailed(quint64 generation, const QString& message);
    void rewatch(const QString& rootPath, bool recursive);

    domain::StemAssociationResolver resolver_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QString watchedRoot_;
    bool watchRecursive_ = false;
    bool watchEnabled_ = false;
    /// Latest generation. Workers compare against it and drop stale results.
    QAtomicInteger<quint64> currentGeneration_ = 0;
    /// Outstanding worker tasks, so destruction can wait for them. Guarded
    /// because workers finish on the pool while the owning thread appends.
    QMutex inFlightGuard_;
    QList<QFuture<void>> inFlight_;
};

} // namespace cullfinch::infrastructure
