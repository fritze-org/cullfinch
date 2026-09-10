// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <CompositionRoot.h>

#include <cullfinch/infrastructure/Paths.h>
#include <cullfinch/testsupport/FakeTrashAdapter.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace cullfinch::guitests {

/// The production composition, wired to a disposable collection, application
/// data root and cache, with a fake Trash adapter.
///
/// GUI tests use the real widgets: nothing here substitutes a simplified view.
class GuiFixture {
public:
    GuiFixture() {
        dataDirectory_ = std::make_unique<QTemporaryDir>();
        cacheDirectory_ = std::make_unique<QTemporaryDir>();

        app::CompositionRoot::Options options;
        options.dataDirectory = dataDirectory_->path();
        options.cacheDirectory = cacheDirectory_->path();
        options.trashAdapter = &trash_;
        root_ = std::make_unique<app::CompositionRoot>(options);
    }

    ~GuiFixture() {
        window_.reset();
        root_.reset();
        infrastructure::Paths::clearOverrides();
    }

    GuiFixture(const GuiFixture&) = delete;
    GuiFixture& operator=(const GuiFixture&) = delete;
    GuiFixture(GuiFixture&&) = delete;
    GuiFixture& operator=(GuiFixture&&) = delete;

    [[nodiscard]] bool initialise(QString* error) { return root_->initialise(error); }

    [[nodiscard]] ui::BrowserWindow* showWindow() {
        window_.reset(root_->createBrowserWindow());
        window_->setStagingRoot(collection_.filePath(QStringLiteral(".cullfinch-staging")));
        window_->show();
        return window_.get();
    }

    /// Open the fixture directory and wait until the scan has published assets.
    [[nodiscard]] bool openCollection(int expectedAssets) {
        QSignalSpy changed(&root_->collection(), &application::CollectionController::assetsChanged);
        if (!window_->openDirectory(collection_.path())) {
            return false;
        }
        // Wait on a specific condition, never an arbitrary sleep.
        const int deadlineMs = 15000;
        QElapsedTimer timer;
        timer.start();
        while (window_->model()->rowCount() != expectedAssets && timer.elapsed() < deadlineMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return window_->model()->rowCount() == expectedAssets;
    }

    [[nodiscard]] testsupport::TempCollection& collection() { return collection_; }
    [[nodiscard]] testsupport::FakeTrashAdapter& trash() { return trash_; }
    [[nodiscard]] app::CompositionRoot& root() { return *root_; }
    [[nodiscard]] ui::BrowserWindow* window() { return window_.get(); }

    /// Wait for a condition with a bounded timeout, processing events.
    template<typename Predicate>
    static bool waitFor(Predicate predicate, int timeoutMs = 10000) {
        QElapsedTimer timer;
        timer.start();
        while (!predicate() && timer.elapsed() < timeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return predicate();
    }

private:
    std::unique_ptr<QTemporaryDir> dataDirectory_;
    std::unique_ptr<QTemporaryDir> cacheDirectory_;
    testsupport::TempCollection collection_;
    testsupport::FakeTrashAdapter trash_;
    std::unique_ptr<app::CompositionRoot> root_;
    std::unique_ptr<ui::BrowserWindow> window_;
};

/// Assert the Qt platform plugin actually in use, so an accidental fallback to
/// offscreen never passes as a desktop-backend run.
inline void requirePlatform(const QString& expected) {
    if (expected.isEmpty()) {
        return;
    }
    QCOMPARE(QGuiApplication::platformName(), expected);
}

} // namespace cullfinch::guitests
