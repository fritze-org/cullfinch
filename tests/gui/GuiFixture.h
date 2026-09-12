// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <CompositionRoot.h>

#include <cullfinch/infrastructure/Paths.h>
#include <cullfinch/testsupport/FakeTrashAdapter.h>
#include <cullfinch/testsupport/TempCollection.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <tuple>

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
        window_ = root_->createBrowserWindow();
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

    /// Test-only: hold decode results (thumbnails included) instead of
    /// delivering them, so a test can capture UI state before a decode a
    /// real window manager triggered during setup would otherwise have
    /// already completed. Safe to call before showWindow().
    void holdImageResults() {
        static_cast<infrastructure::QtImageService&>(root_->images()).holdResultsForTesting();
    }
    void releaseImageResults() {
        static_cast<infrastructure::QtImageService&>(root_->images())
            .releaseHeldResultsForTesting();
    }

    [[nodiscard]] testsupport::TempCollection& collection() { return collection_; }
    /// The application data root this composition writes to. A second
    /// composition against the same root is what a second launch is.
    [[nodiscard]] QString dataDirectory() const { return dataDirectory_->path(); }
    [[nodiscard]] QString cacheDirectory() const { return cacheDirectory_->path(); }
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

/// Give a window a bounded chance to be exposed, without requiring it.
///
/// Exposure depends on the compositor or window manager granting it, and a
/// headless reference session may never do so -- the same reason these suites
/// do not assert keyboard focus. The widget behaviour under test does not need
/// a mapped window, and QTest delivers events to widgets regardless. Tests that
/// genuinely depend on presentation belong in the compositor integration suite.
inline void settleWindow(QWidget* window) {
    if (window == nullptr) {
        return;
    }
    std::ignore = QTest::qWaitForWindowExposed(window, 5000);
    QCoreApplication::processEvents();
}

/// Resize a window and wait until the size it is laid out at is the one asked
/// for.
///
/// resize() only asks. On X11 the window manager answers with a
/// ConfigureNotify that arrives after the call returns, and when the request
/// races the window being mapped, that answer can carry the size the manager
/// mapped it at instead. Qt adopts the requested size immediately, so size()
/// cannot tell a granted request from one about to be taken back; the revert
/// surfaces later as a relayout, moving widgets a test has already measured.
/// That is precisely how the wall suite failed under X11 and nowhere else:
/// tile geometry read before the revert, compared after it.
///
/// So the request is repeated whenever it is taken back, and accepted only
/// once it has held still. Bounded, and in itself never fatal, like
/// settleWindow(): a window manager is entitled to refuse a size. A caller
/// whose measurements only mean something at that size should assert the
/// result, as the wall suite does -- "the window never settled" is a far
/// better failure than two geometries that disagree for no stated reason.
inline bool settleWindowSize(QWidget* window, const QSize& size, int timeoutMs = 5000) {
    if (window == nullptr) {
        return false;
    }

    // One budget for the whole operation: waiting for exposure is part of it,
    // not extra time on top of it.
    QElapsedTimer overall;
    overall.start();

    window->resize(size);
    std::ignore = QTest::qWaitForWindowExposed(window, timeoutMs);

    // Comfortably more than a round trip to the display server under load,
    // which is what this is buying; the dozen calls the wall suite makes cost
    // it a few seconds in total.
    const int holdMs = 250;
    QElapsedTimer held;
    held.start();
    while (overall.elapsed() < timeoutMs) {
        if (window->size() != size) {
            window->resize(size);
            held.restart();
        } else if (held.elapsed() >= holdMs) {
            break;
        }
        QTest::qWait(10);
    }
    QCoreApplication::processEvents();
    return window->size() == size;
}

/// Assert the Qt platform plugin actually in use, so an accidental fallback to
/// offscreen never passes as a desktop-backend run.
inline void requirePlatform(const QString& expected) {
    if (expected.isEmpty()) {
        return;
    }
    QCOMPARE(QGuiApplication::platformName(), expected);
}

} // namespace cullfinch::guitests
