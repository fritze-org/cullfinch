// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/ui/AssetListModel.h>

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QStringList>

namespace cullfinch::ui {

AssetListModel::AssetListModel(application::IImageService& images, QObject* parent)
    : QAbstractListModel(parent), images_(images) {
    connect(&images_, &application::IImageService::imageReady, this, &AssetListModel::onImageReady);
}

void AssetListModel::setAssets(const domain::PhotoAssetList& assets, quint64 generation) {
    beginResetModel();
    assets_ = assets;
    generation_ = generation;
    rowById_.clear();
    for (int row = 0; row < assets_.size(); ++row) {
        rowById_.insert(assets_.at(row).id, row);
    }
    // Thumbnails stay keyed by member identity and fingerprint, so unchanged
    // photos keep their images across a rescan.
    requested_.clear();
    endResetModel();
}

int AssetListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(assets_.size());
}

int AssetListModel::rowForId(const domain::AssetId& id) const {
    return rowById_.value(id, -1);
}

domain::AssetId AssetListModel::idForRow(int row) const {
    if (row < 0 || row >= assets_.size()) {
        return {};
    }
    return assets_.at(row).id;
}

domain::PhotoAssetList AssetListModel::assetsForIds(const QList<domain::AssetId>& ids) const {
    domain::PhotoAssetList result;
    result.reserve(ids.size());
    for (const domain::AssetId& id : ids) {
        const int row = rowForId(id);
        if (row >= 0) {
            result.append(assets_.at(row));
        }
    }
    return result;
}

void AssetListModel::setThumbnailSize(const QSize& size) {
    if (thumbnailSize_ == size) {
        return;
    }
    beginResetModel();
    thumbnailSize_ = size;
    thumbnails_.clear();
    requested_.clear();
    endResetModel();
}

Qt::ItemFlags AssetListModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> AssetListModel::roleNames() const {
    QHash<int, QByteArray> names = QAbstractListModel::roleNames();
    names.insert(AssetIdRole, QByteArrayLiteral("assetId"));
    names.insert(DisplayNameRole, QByteArrayLiteral("displayName"));
    names.insert(PairingTextRole, QByteArrayLiteral("pairingText"));
    names.insert(RawCountRole, QByteArrayLiteral("rawCount"));
    names.insert(DispositionRole, QByteArrayLiteral("disposition"));
    names.insert(ComparableRole, QByteArrayLiteral("comparable"));
    names.insert(OperableRole, QByteArrayLiteral("operable"));
    return names;
}

QVariant AssetListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= assets_.size()) {
        return {};
    }
    const domain::PhotoAsset& asset = assets_.at(index.row());

    switch (role) {
    case Qt::DisplayRole: {
        QString label = asset.displayName;
        if (asset.rawCount() > 0) {
            // A RAW badge as text, not colour alone.
            label += QStringLiteral(" [RAW ×%1]").arg(asset.rawCount());
        }
        if (asset.disposition == domain::Disposition::Reject) {
            label = tr("%1 — marked for deletion").arg(label);
        }
        return label;
    }
    case Qt::ToolTipRole: {
        QStringList lines{asset.displayName, domain::pairingStateName(asset.pairingState),
                          tr("%1 file(s)").arg(asset.members.size())};
        lines.append(asset.diagnostics);
        return lines.join(QLatin1Char('\n'));
    }
    case Qt::AccessibleTextRole:
        return tr("%1, %2, %3 files%4")
            .arg(asset.displayName, domain::pairingStateName(asset.pairingState))
            .arg(asset.members.size())
            .arg(asset.disposition == domain::Disposition::Reject ? tr(", marked for deletion")
                                                                  : QString());
    case Qt::DecorationRole: {
        const domain::FileMember* preview = asset.preview();
        if (preview == nullptr) {
            return {};
        }
        const auto cached = thumbnails_.constFind(preview->id);
        if (cached != thumbnails_.constEnd()) {
            return QIcon(QPixmap::fromImage(*cached));
        }
        requestThumbnail(index.row());
        return {};
    }
    case AssetIdRole:
        return QVariant::fromValue(asset.id);
    case DisplayNameRole:
        return asset.displayName;
    case PairingStateRole:
        return static_cast<int>(asset.pairingState);
    case PairingTextRole:
        return domain::pairingStateName(asset.pairingState);
    case RawCountRole:
        return asset.rawCount();
    case MemberCountRole:
        return static_cast<int>(asset.members.size());
    case DispositionRole:
        return static_cast<int>(asset.disposition);
    case DiagnosticsRole:
        return asset.diagnostics;
    case ComparableRole:
        return asset.isComparable();
    case OperableRole:
        return asset.isOperable();
    case PreviewPathRole: {
        const domain::FileMember* preview = asset.preview();
        return preview != nullptr ? preview->absolutePath : QString();
    }
    default:
        break;
    }
    return {};
}

void AssetListModel::requestThumbnail(int row) const {
    const domain::PhotoAsset& asset = assets_.at(row);
    const domain::FileMember* preview = asset.preview();
    if (preview == nullptr || requested_.value(preview->id, false)) {
        return;
    }
    requested_.insert(preview->id, true);

    application::ImageRequest request;
    request.memberId = preview->id;
    request.path = preview->absolutePath;
    request.fingerprint = preview->fingerprint;
    request.targetSize = thumbnailSize_;
    request.kind = application::ImageRequestClass::Thumbnail;
    request.priority = 10; // Browser prefetch has the lowest priority.
    request.generation = generation_;
    images_.request(request);
}

void AssetListModel::onImageReady(const application::ImageResult& result) {
    if (result.kind != application::ImageRequestClass::Thumbnail || !result.success) {
        // A failed thumbnail leaves a placeholder; it never changes asset order.
        return;
    }
    if (result.generation != generation_) {
        return;
    }

    thumbnails_.insert(result.memberId, result.image);
    for (int row = 0; row < assets_.size(); ++row) {
        const domain::FileMember* preview = assets_.at(row).preview();
        if (preview != nullptr && preview->id == result.memberId) {
            const QModelIndex changed = index(row, 0);
            Q_EMIT dataChanged(changed, changed, {Qt::DecorationRole});
            return;
        }
    }
}

} // namespace cullfinch::ui
