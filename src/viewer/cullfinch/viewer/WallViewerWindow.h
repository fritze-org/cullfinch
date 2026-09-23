// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/ImageService.h>
#include <cullfinch/viewer/DirectoryPhotos.h>
#include <cullfinch/views/wall/WallView.h>

#include <QAction>
#include <QLabel>
#include <QPoint>
#include <QScrollArea>
#include <QSlider>
#include <QToolBar>
#include <QWidget>

namespace cullfinch::viewer {

/// Every photo in one directory on one wall, to look at and nothing else.
///
/// The comparison wall's surface, without the comparison: no flow, no session,
/// no draft and no marks, so a click or Delete on a tile decides nothing and
/// nothing on disk can change. What it adds is a tile size. The comparison wall
/// has to fit every candidate on one screen, because a photo scrolled out of
/// sight cannot be compared; a viewer has no such rule, so it can offer tiles
/// of any size and scroll.
class WallViewerWindow : public QWidget {
    Q_OBJECT

public:
    /// The slider's range. Below the minimum a caption no longer fits under the
    /// photo; above the maximum a single tile fills most screens anyway.
    static constexpr int kMinimumTileWidth = 96;
    static constexpr int kMaximumTileWidth = 1024;

    /// @param tileWidth the size to start at, as for setTileWidth(). It is a
    ///        constructor argument because it decides which tiles decode: a
    ///        window built fitted and resized afterwards would already have
    ///        asked for every photo in the directory.
    WallViewerWindow(application::IImageService& images, const QString& directory,
                     const DirectoryPhotos& photos, int tileWidth = 0, QWidget* parent = nullptr);

    /// Tiles about `pixels` wide in a scrolling grid, or zero to fit every
    /// photo into the window as the comparison wall does. Clamped to the
    /// slider's range.
    void setTileWidth(int pixels);
    [[nodiscard]] int tileWidth() const;

    /// Grow (positive) or shrink (negative) the tiles by `steps` notches,
    /// keeping the photo under `anchor`, in viewport coordinates, at the same
    /// height on screen. It may change column -- the grid never scrolls
    /// sideways -- but it does not leave the screen. From Fit, the first step
    /// starts at the size the tiles already have, so nothing jumps.
    void zoomTiles(int steps, const QPoint& anchor);

    void toggleFullscreen();

    [[nodiscard]] views::wall::WallSurface* surface() const { return surface_; }
    [[nodiscard]] QScrollArea* scrollArea() const { return scroll_; }

signals:
    /// The tile width somebody chose, zero meaning Fit, for the caller to
    /// remember. The window itself never touches settings, so a test cannot
    /// change a user's.
    void tileWidthChanged(int pixels);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildControls(const DirectoryPhotos& photos);
    /// Apply a width while keeping the photo under `anchor` in place.
    void applyTileWidth(int pixels, const QPoint& anchor);
    /// The width the tiles have on screen now, whether chosen or fitted.
    [[nodiscard]] int effectiveTileWidth() const;
    void syncControls();

    views::wall::WallSurface* surface_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    QToolBar* strip_ = nullptr;
    QSlider* size_ = nullptr;
    QAction* fitAction_ = nullptr;
    QAction* largerAction_ = nullptr;
    QAction* smallerAction_ = nullptr;
    QAction* fullscreenAction_ = nullptr;
    QLabel* emptyLabel_ = nullptr;

    /// A high-resolution wheel reports fractions of a notch; they add up here
    /// until they make a whole step.
    int wheelRemainder_ = 0;
    bool syncing_ = false;
};

} // namespace cullfinch::viewer
