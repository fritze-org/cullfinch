// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>
#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/domain/PhotoAsset.h>

#include <QObject>
#include <QString>
#include <QStringList>

namespace cullfinch::application {

/// One asynchronous directory scan.
struct ScanRequest {
    domain::CollectionId collectionId;
    QString rootPath;
    bool recursive = false;
    domain::AssociationConfig config;
    /// Bumped for every new scan. Results carrying an old generation are
    /// discarded, so a superseded directory can never repopulate the browser.
    quint64 generation = 0;
};

/// Asynchronous filesystem discovery. Implemented in the infrastructure layer.
class IScanService : public QObject {
    Q_OBJECT

public:
    explicit IScanService(QObject* parent = nullptr) : QObject(parent) {}

    virtual void requestScan(const ScanRequest& request) = 0;
    /// Stop watching and abandon in-flight work for every generation.
    virtual void cancelAll() = 0;
    /// Start or stop reporting external filesystem changes for the scan root.
    virtual void setWatchEnabled(bool enabled) = 0;

signals:
    /// Partial results, published in batches while a scan runs. Every asset in
    /// a batch is Provisional.
    void scanBatch(quint64 generation, const cullfinch::domain::PhotoAssetList& assets);
    void scanFinished(quint64 generation, const cullfinch::domain::AssociationResult& result);
    void scanFailed(quint64 generation, const QString& message);
    /// A watcher hint, not an authoritative inventory: the controller reacts by
    /// rescanning the affected scope.
    void externalChangeDetected(const QString& path);
};

/// Coordinates scanning, reconciliation and the in-memory asset set.
class CollectionController : public QObject {
    Q_OBJECT

public:
    CollectionController(IAssetRepository& repository, IScanService& scanner,
                         QObject* parent = nullptr);

    bool open(const QString& rootPath, bool recursive, QString* error);
    void refresh();
    void close();

    [[nodiscard]] domain::CollectionId collectionId() const { return collectionId_; }
    [[nodiscard]] quint64 revision() const { return revision_; }
    [[nodiscard]] QString rootPath() const { return rootPath_; }
    [[nodiscard]] bool isScanning() const { return scanning_; }

    [[nodiscard]] const domain::PhotoAssetList& assets() const { return assets_; }
    [[nodiscard]] const domain::PhotoAsset* asset(const domain::AssetId& id) const;
    [[nodiscard]] domain::PhotoAssetList assetsByIds(const QList<domain::AssetId>& ids) const;
    [[nodiscard]] domain::PhotoAssetList rejectedAssets() const;
    [[nodiscard]] int rejectedCount() const;

    [[nodiscard]] domain::AssociationConfig associationConfig() const { return config_; }
    void setAssociationConfig(const domain::AssociationConfig& config);

    /// Applied after a disposition change so the in-memory set matches storage
    /// without a full rescan.
    void applyDispositionChange(const QList<domain::AssetId>& affected, quint64 revision);

signals:
    void collectionOpened(const QString& rootPath);
    /// The stored collection revision advanced. Anything holding an
    /// expected revision for optimistic concurrency has to follow it, or
    /// its next write is refused as a phantom conflict.
    void revisionChanged(quint64 revision);
    void assetsChanged();
    void scanStateChanged(bool scanning);
    void errorOccurred(const QString& message);
    void diagnosticsChanged(const QStringList& diagnostics);

private:
    void startScan();

    IAssetRepository& repository_;
    IScanService& scanner_;

    domain::CollectionId collectionId_;
    QString rootPath_;
    bool recursive_ = false;
    quint64 revision_ = 0;
    quint64 generation_ = 0;
    bool scanning_ = false;

    domain::AssociationConfig config_ = domain::AssociationConfig::defaults();
    domain::PhotoAssetList assets_;
};

} // namespace cullfinch::application
