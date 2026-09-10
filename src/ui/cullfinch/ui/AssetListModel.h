// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/domain/PhotoAsset.h>

#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QSize>

namespace cullfinch::ui {

/// The browser's collection model.
///
/// Rows are a presentation detail: every long-lived reference is an AssetId.
class AssetListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        AssetIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        PairingStateRole,
        PairingTextRole,
        RawCountRole,
        MemberCountRole,
        DispositionRole,
        DiagnosticsRole,
        ComparableRole,
        OperableRole,
        PreviewPathRole
    };
    Q_ENUM(Role)

    explicit AssetListModel(application::IImageService& images, QObject* parent = nullptr);

    void setAssets(const domain::PhotoAssetList& assets, quint64 generation);
    [[nodiscard]] const domain::PhotoAssetList& assets() const { return assets_; }
    [[nodiscard]] domain::PhotoAssetList assetsForIds(const QList<domain::AssetId>& ids) const;
    [[nodiscard]] int rowForId(const domain::AssetId& id) const;
    [[nodiscard]] domain::AssetId idForRow(int row) const;

    void setThumbnailSize(const QSize& size);
    [[nodiscard]] QSize thumbnailSize() const { return thumbnailSize_; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    void requestThumbnail(int row) const;
    void onImageReady(const application::ImageResult& result);

    application::IImageService& images_;
    domain::PhotoAssetList assets_;
    QHash<domain::AssetId, int> rowById_;
    mutable QHash<domain::MemberId, QImage> thumbnails_;
    mutable QHash<domain::MemberId, bool> requested_;
    QSize thumbnailSize_{192, 192};
    quint64 generation_ = 0;
};

} // namespace cullfinch::ui
