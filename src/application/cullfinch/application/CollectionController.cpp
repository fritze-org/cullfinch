// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/application/CollectionController.h>

#include <QCoreApplication>
#include <QHash>

namespace cullfinch::application {
CollectionController::CollectionController(IAssetRepository& repository, IScanService& scanner,
                                           QObject* parent)
    : QObject(parent), repository_(repository), scanner_(scanner) {
    connect(&scanner_, &IScanService::scanBatch, this,
            [this](quint64 generation, const domain::PhotoAssetList& batch) {
                if (generation != generation_) {
                    return; // A superseded directory or selection.
                }
                assets_ = batch;
                Q_EMIT assetsChanged();
            });

    connect(&scanner_, &IScanService::scanFinished, this,
            [this](quint64 generation, const domain::AssociationResult& result) {
                if (generation != generation_) {
                    return;
                }
                scanning_ = false;

                domain::PhotoAssetList merged;
                quint64 newRevision = revision_;
                QString error;
                if (!repository_.reconcileAssets(collectionId_, result.assets, &merged,
                                                 &newRevision, &error)) {
                    Q_EMIT errorOccurred(tr("The scan results could not be stored: %1").arg(error));
                    // The in-memory view still reflects the filesystem, but
                    // marks and drafts stay with what storage last accepted.
                    assets_ = result.assets;
                } else {
                    assets_ = merged;
                    revision_ = newRevision;
                }

                Q_EMIT diagnosticsChanged(result.diagnostics);
                Q_EMIT assetsChanged();
                Q_EMIT scanStateChanged(false);
            });

    connect(&scanner_, &IScanService::scanFailed, this,
            [this](quint64 generation, const QString& message) {
                if (generation != generation_) {
                    return;
                }
                scanning_ = false;
                Q_EMIT errorOccurred(message);
                Q_EMIT scanStateChanged(false);
            });

    // A watcher is a hint, so the response is a rescan of the affected scope
    // rather than a direct edit of the asset set.
    connect(&scanner_, &IScanService::externalChangeDetected, this,
            [this](const QString&) { refresh(); });
}

bool CollectionController::open(const QString& rootPath, bool recursive, QString* error) {
    QString storageError;
    const std::optional<domain::CollectionId> id =
        repository_.ensureCollection(rootPath, recursive, &storageError);
    if (!id.has_value()) {
        if (error != nullptr) {
            *error = storageError;
        }
        return false;
    }

    collectionId_ = *id;
    rootPath_ = rootPath;
    recursive_ = recursive;
    revision_ = repository_.collectionRevision(collectionId_, &storageError);
    assets_ = repository_.loadAssets(collectionId_, &storageError);

    Q_EMIT collectionOpened(rootPath_);
    Q_EMIT assetsChanged();
    startScan();
    scanner_.setWatchEnabled(true);
    return true;
}

void CollectionController::refresh() {
    if (!collectionId_.isValid()) {
        return;
    }
    startScan();
}

void CollectionController::close() {
    scanner_.setWatchEnabled(false);
    scanner_.cancelAll();
    ++generation_; // Discard anything still in flight.
    collectionId_ = domain::CollectionId{};
    rootPath_.clear();
    assets_.clear();
    revision_ = 0;
    scanning_ = false;
    Q_EMIT assetsChanged();
    Q_EMIT scanStateChanged(false);
}

void CollectionController::startScan() {
    ++generation_;
    scanning_ = true;
    Q_EMIT scanStateChanged(true);

    ScanRequest request;
    request.collectionId = collectionId_;
    request.rootPath = rootPath_;
    request.recursive = recursive_;
    request.config = config_;
    request.generation = generation_;
    scanner_.requestScan(request);
}

void CollectionController::setAssociationConfig(const domain::AssociationConfig& config) {
    config_ = config;
    refresh();
}

const domain::PhotoAsset* CollectionController::asset(const domain::AssetId& id) const {
    for (const domain::PhotoAsset& candidate : assets_) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

domain::PhotoAssetList CollectionController::assetsByIds(const QList<domain::AssetId>& ids) const {
    QHash<domain::AssetId, const domain::PhotoAsset*> index;
    index.reserve(assets_.size());
    for (const domain::PhotoAsset& candidate : assets_) {
        index.insert(candidate.id, &candidate);
    }

    domain::PhotoAssetList result;
    result.reserve(ids.size());
    for (const domain::AssetId& id : ids) {
        const domain::PhotoAsset* found = index.value(id, nullptr);
        if (found != nullptr) {
            result.append(*found);
        }
    }
    return result;
}

domain::PhotoAssetList CollectionController::rejectedAssets() const {
    domain::PhotoAssetList result;
    for (const domain::PhotoAsset& candidate : assets_) {
        if (candidate.disposition == domain::Disposition::Reject) {
            result.append(candidate);
        }
    }
    return result;
}

int CollectionController::rejectedCount() const {
    return static_cast<int>(rejectedAssets().size());
}

void CollectionController::applyDispositionChange(const QList<domain::AssetId>& affected,
                                                  quint64 revision) {
    QString error;
    const domain::PhotoAssetList stored = repository_.loadAssets(collectionId_, &error);
    if (stored.isEmpty() && !assets_.isEmpty()) {
        Q_EMIT errorOccurred(tr("Deletion marks changed but could not be re-read: %1").arg(error));
        return;
    }
    Q_UNUSED(affected)
    assets_ = stored;
    revision_ = revision;
    Q_EMIT assetsChanged();
}

} // namespace cullfinch::application
