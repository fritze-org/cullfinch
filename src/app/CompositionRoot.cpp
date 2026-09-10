// SPDX-License-Identifier: GPL-3.0-or-later
#include <CompositionRoot.h>

#include <cullfinch/flows/versus/VersusFlow.h>
#include <cullfinch/flows/wall/WallFlow.h>
#include <cullfinch/infrastructure/Paths.h>
#include <cullfinch/views/versus/VersusView.h>
#include <cullfinch/views/wall/WallView.h>

namespace cullfinch::app {

CompositionRoot::CompositionRoot() : CompositionRoot(Options{}) {}

CompositionRoot::CompositionRoot(const Options& options) : options_(options) {
    if (!options_.dataDirectory.isEmpty() || !options_.cacheDirectory.isEmpty()) {
        infrastructure::Paths::overrideRoots(options_.dataDirectory, options_.cacheDirectory);
    }
}

CompositionRoot::~CompositionRoot() = default;

bool CompositionRoot::initialise(QString* error) {
    repository_ =
        std::make_unique<infrastructure::SqliteRepository>(infrastructure::Paths::databaseFile());
    if (!repository_->open(error)) {
        return false;
    }

    scanner_ = std::make_unique<infrastructure::DirectoryScanner>();
    images_ = std::make_unique<infrastructure::QtImageService>();
    images_->setMemoryBudgetBytes(options_.imageMemoryBudgetBytes);

    application::ITrashAdapter* trash = options_.trashAdapter;
    if (trash == nullptr) {
        ownedTrash_ = std::make_unique<infrastructure::QtTrashAdapter>();
        trash = ownedTrash_.get();
    }
    executor_ = std::make_unique<infrastructure::StagingExecutor>(*trash);

    collection_ = std::make_unique<application::CollectionController>(*repository_, *scanner_);
    dispositions_ = std::make_unique<application::DispositionController>(*repository_);
    session_ =
        std::make_unique<application::SessionController>(flows_, *repository_, *dispositions_);
    operations_ = std::make_unique<application::OperationController>(*repository_, *executor_);

    // The disposition controller follows whichever collection is open.
    QObject::connect(collection_.get(), &application::CollectionController::collectionOpened,
                     dispositions_.get(), [this](const QString&) {
                         dispositions_->setCollection(collection_->collectionId(),
                                                      collection_->revision());
                     });

    registerFlows();
    return true;
}

void CompositionRoot::registerFlows() {
    // Engine and view register under the same stable identifier. Nothing else
    // in the application needs to know these names.
    flows_.registerFlow(flows::versus::VersusFlow{}.descriptor(),
                        []() { return std::make_unique<flows::versus::VersusFlow>(); });
    flowViews_.registerView(QString::fromLatin1(flows::versus::kFlowId),
                            [](application::IImageService& images) {
                                return std::make_unique<views::versus::VersusView>(images);
                            });

    flows_.registerFlow(flows::wall::WallFlow{}.descriptor(),
                        []() { return std::make_unique<flows::wall::WallFlow>(); });
    flowViews_.registerView(QString::fromLatin1(flows::wall::kFlowId),
                            [](application::IImageService& images) {
                                return std::make_unique<views::wall::WallView>(images);
                            });
}

ui::BrowserWindow* CompositionRoot::createBrowserWindow() {
    const ui::AppContext context{*repository_, *collection_, *session_,  *dispositions_,
                                 *operations_, flows_,       flowViews_, *images_};
    return new ui::BrowserWindow(context);
}

} // namespace cullfinch::app
