// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/CollectionController.h>
#include <cullfinch/application/DispositionController.h>
#include <cullfinch/application/FlowRegistry.h>
#include <cullfinch/application/OperationController.h>
#include <cullfinch/application/SessionController.h>
#include <cullfinch/infrastructure/AppLock.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>
#include <cullfinch/infrastructure/QtImageService.h>
#include <cullfinch/infrastructure/SqliteRepository.h>
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/infrastructure/TrashAdapter.h>
#include <cullfinch/ui/BrowserWindow.h>
#include <cullfinch/ui/FlowView.h>

#include <QString>

#include <memory>

namespace cullfinch::app {

/// Assembles the application from its parts.
///
/// This is the only place that knows every concrete adapter. Flows and their
/// views are registered here under matching identifiers; adding a comparison
/// mode means adding two registrations and its tests, nothing more.
class CompositionRoot {
public:
    struct Options {
        /// Override the metadata and cache roots. Tests and `--data-dir` use
        /// this so a run never touches the developer's own database.
        QString dataDirectory;
        QString cacheDirectory;
        /// Replace the real Trash with a fake. GUI tests always do.
        application::ITrashAdapter* trashAdapter = nullptr;
        qint64 imageMemoryBudgetBytes = 512LL * 1024 * 1024;
    };

    /// Two constructors rather than a defaulted argument: a nested aggregate's
    /// default member initializers are not usable until the enclosing class is
    /// complete, so `= {}` here is ill-formed.
    CompositionRoot();
    explicit CompositionRoot(const Options& options);
    ~CompositionRoot();

    CompositionRoot(const CompositionRoot&) = delete;
    CompositionRoot& operator=(const CompositionRoot&) = delete;
    CompositionRoot(CompositionRoot&&) = delete;
    CompositionRoot& operator=(CompositionRoot&&) = delete;

    /// @return false when the metadata database cannot be opened.
    bool initialise(QString* error);

    /// Owned by the caller after this call; created on first use.
    [[nodiscard]] ui::BrowserWindow* createBrowserWindow();

    [[nodiscard]] application::FlowRegistry& flows() { return flows_; }
    [[nodiscard]] ui::FlowViewRegistry& flowViews() { return flowViews_; }
    [[nodiscard]] application::IAssetRepository& repository() { return *repository_; }
    [[nodiscard]] application::CollectionController& collection() { return *collection_; }
    [[nodiscard]] application::SessionController& session() { return *session_; }
    [[nodiscard]] application::DispositionController& dispositions() { return *dispositions_; }
    [[nodiscard]] application::OperationController& operations() { return *operations_; }
    [[nodiscard]] application::IImageService& images() { return *images_; }

private:
    void registerFlows();

    Options options_;

    std::unique_ptr<infrastructure::SqliteRepository> repository_;
    std::unique_ptr<infrastructure::DirectoryScanner> scanner_;
    std::unique_ptr<infrastructure::QtImageService> images_;
    std::unique_ptr<infrastructure::QtTrashAdapter> ownedTrash_;
    std::unique_ptr<infrastructure::StagingExecutor> executor_;
    std::unique_ptr<infrastructure::AppLock> lock_;

    application::FlowRegistry flows_;
    ui::FlowViewRegistry flowViews_;

    std::unique_ptr<application::CollectionController> collection_;
    std::unique_ptr<application::DispositionController> dispositions_;
    std::unique_ptr<application::SessionController> session_;
    std::unique_ptr<application::OperationController> operations_;
};

} // namespace cullfinch::app
