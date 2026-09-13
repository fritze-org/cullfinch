// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/CollectionController.h>
#include <cullfinch/domain/AssociationPolicy.h>

#include <QFileSystemWatcher>
#include <QStringList>
#include <QTimer>

#include <memory>

namespace cullfinch::infrastructure {

class ScanState;

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
    void rewatch(const QString& rootPath, bool recursive);

    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QString watchedRoot_;
    bool watchRecursive_ = false;
    bool watchEnabled_ = false;

    /// Current generation and worker-delivery signals, held by shared_ptr
    /// rather than as plain members. A worker task captures this pointer, not
    /// `this`: it never dereferences the scanner, so a scanner destroyed
    /// mid-scan just drops its own reference while the worker's copy keeps
    /// the state alive until the task finishes. Delivery back to the GUI
    /// thread is a queued signal from that shared object, so Qt only needs
    /// the state to be alive to decide whether the connection still stands.
    std::shared_ptr<ScanState> state_;
};

} // namespace cullfinch::infrastructure
