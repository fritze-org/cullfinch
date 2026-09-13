// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <cullfinch/infrastructure/Paths.h>

#include <QAtomicInteger>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <tuple>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace cullfinch::infrastructure {

/// Generation counter and delivery signals shared between a scanner and the
/// worker task(s) it has in flight.
///
/// Owned jointly by the scanner and by every task it has started, via
/// std::shared_ptr: a task captures a copy of this object instead of `this`,
/// so it never touches the scanner and a scanner destroyed mid-scan cannot
/// leave a task holding a dangling pointer. Delivery is a queued signal
/// rather than a direct call for the same reason -- emitting only requires
/// this (independently-alive) object to still exist, and Qt tears the
/// connection to the scanner down under its own lock if the scanner is
/// destroyed first, so a task never has to know whether the scanner is still
/// there.
class ScanState : public QObject {
    Q_OBJECT

public:
    /// Latest generation. Workers compare against it and drop stale results.
    QAtomicInteger<quint64> generation{0};

Q_SIGNALS:
    /// A task's result, for the scanner to check against the current
    /// generation and re-emit as scanFinished if it still applies.
    void finished(quint64 generation, const domain::AssociationResult& result);
    /// A task's error, for the scanner to check against the current
    /// generation and re-emit as scanFailed if it still applies.
    void failed(quint64 generation, const QString& message);
};

namespace {

/// Native identity, where the platform offers one. Used to notice replaced
/// files and hardlink aliases; never to merge assets.
domain::NativeIdentity nativeIdentityOf(const QFileInfo& info) {
    domain::NativeIdentity identity;
#if defined(Q_OS_UNIX)
    // lstat, not stat: a symlink reports its own identity so it is never
    // mistaken for the file it points at.
    struct stat status = {};
    if (const QByteArray encoded = QFile::encodeName(info.absoluteFilePath());
        ::lstat(encoded.constData(), &status) == 0) {
        identity.device = static_cast<quint64>(status.st_dev);
        identity.fileId = static_cast<quint64>(status.st_ino);
        identity.known = true;
    }
#else
    Q_UNUSED(info)
#endif
    return identity;
}

domain::DiscoveredFile describe(const QFileInfo& info, const QString& rootPath) {
    domain::DiscoveredFile file;
    file.absolutePath = info.absoluteFilePath();
    file.fileName = info.fileName();
    file.stem = info.completeBaseName();
    file.extension = info.suffix();
    file.isSymlink = info.isSymLink();

    const QString relative = QDir(rootPath).relativeFilePath(info.absolutePath());
    file.relativeDirectory = (relative == QLatin1String(".")) ? QString() : relative;

    file.fingerprint.sizeBytes = info.size();
    file.fingerprint.modifiedMsecsUtc = info.lastModified().toMSecsSinceEpoch();
    file.fingerprint.native = nativeIdentityOf(info);
    return file;
}

} // namespace

DirectoryScanner::DirectoryScanner(QObject* parent)
    : application::IScanService(parent),
      // A worker task may be the one to drop the last reference to state_ (a
      // scanner destroyed mid-scan while a task is still running). state_
      // lives on this thread, so it must not be deleted on the pool thread
      // that happens to release it; deleteLater() always runs the actual
      // delete on the object's own thread, regardless of which thread calls
      // it.
      state_(new ScanState, [](ScanState* state) { state->deleteLater(); }) {
    debounce_.setSingleShot(true);
    // Watcher notifications arrive in bursts; one rescan per burst is enough.
    debounce_.setInterval(400);
    connect(&debounce_, &QTimer::timeout, this,
            [this]() { Q_EMIT externalChangeDetected(watchedRoot_); });

    const auto onChange = [this](const QString&) {
        if (watchEnabled_ && !debounce_.isActive()) {
            debounce_.start();
        }
    };
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, onChange);
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, onChange);

    // Both ends are alive here, but that is not what later makes this safe:
    // emitting from state_ on a worker thread only ever locks state_'s own
    // connection list, and ~DirectoryScanner tears this connection down
    // under that same lock, so a worker emitting after the scanner is gone
    // simply finds no connection left rather than a dangling receiver.
    connect(
        state_.get(), &ScanState::finished, this,
        [this](quint64 generation, const domain::AssociationResult& result) {
            if (state_->generation.loadRelaxed() != generation) {
                return; // Superseded before delivery.
            }
            Q_EMIT scanFinished(generation, result);
        },
        Qt::QueuedConnection);
    connect(
        state_.get(), &ScanState::failed, this,
        [this](quint64 generation, const QString& message) {
            if (state_->generation.loadRelaxed() != generation) {
                return;
            }
            Q_EMIT scanFailed(generation, message);
        },
        Qt::QueuedConnection);
}

// state_ is a shared_ptr: any task still running when the scanner goes away
// keeps it alive through its own copy, so there is nothing to wait for here.
DirectoryScanner::~DirectoryScanner() = default;

QList<domain::DiscoveredFile> DirectoryScanner::enumerate(const QString& rootPath, bool recursive,
                                                          QString* error) {
    QList<domain::DiscoveredFile> files;

    const QFileInfo rootInfo(rootPath);
    if (!rootInfo.isDir()) {
        if (error != nullptr) {
            *error = tr("'%1' is not a directory.").arg(rootPath);
        }
        return files;
    }
    if (!rootInfo.isReadable()) {
        if (error != nullptr) {
            *error = tr("'%1' cannot be read.").arg(rootPath);
        }
        return files;
    }

    const QString stagingName = Paths::stagingDirectoryName();
    const QDirIterator::IteratorFlags flags =
        recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;

    // Symlinked directories are not followed: a second representation of the
    // same tree must never be operated on twice.
    QDirIterator iterator(rootPath, QDir::Files | QDir::NoDotAndDotDot, flags);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.absoluteFilePath().contains(QLatin1Char('/') + stagingName + QLatin1Char('/'))) {
            continue; // The operation staging root is not part of the collection.
        }
        files.append(describe(info, rootPath));
    }

    return files;
}

void DirectoryScanner::requestScan(const application::ScanRequest& request) {
    state_->generation.storeRelaxed(request.generation);
    rewatch(request.rootPath, request.recursive);

    const quint64 generation = request.generation;
    const QString rootPath = request.rootPath;
    const bool recursive = request.recursive;
    const domain::AssociationConfig config = request.config;
    const domain::CollectionId collectionId = request.collectionId;
    const std::shared_ptr<ScanState> state = state_;

    // Decode-free work, so QThreadPool is enough; the scanner never holds a
    // widget pointer. The task below captures state, never `this` or the
    // scanner's resolver_: it must be safe to run for as long as it likes
    // after requestScan() returns, including past the scanner's destruction.
    std::ignore =
        QtConcurrent::run(QThreadPool::globalInstance(), [state, generation, rootPath, recursive,
                                                          config, collectionId]() {
            QString error;
            const QList<domain::DiscoveredFile> files = enumerate(rootPath, recursive, &error);
            if (state->generation.loadRelaxed() != generation) {
                return; // Superseded while enumerating; nothing to deliver.
            }
            if (!error.isEmpty()) {
                Q_EMIT state->failed(generation, error);
                return;
            }

            // Stateless, so built fresh here rather than borrowed from the
            // scanner, which this task must never touch.
            const domain::StemAssociationResolver resolver;
            // Classification happens only once enumeration of the association scope
            // has finished for this generation.
            const domain::AssociationResult result =
                resolver.resolve(collectionId, files, config, /*scopeComplete=*/true);
            if (state->generation.loadRelaxed() != generation) {
                return; // Superseded while resolving; nothing to deliver.
            }
            Q_EMIT state->finished(generation, result);
        });
}

void DirectoryScanner::rewatch(const QString& rootPath, bool recursive) {
    watchedRoot_ = rootPath;
    watchRecursive_ = recursive;
    if (!watchEnabled_) {
        return;
    }

    if (const QStringList watchedDirectories = watcher_.directories();
        !watchedDirectories.isEmpty()) {
        watcher_.removePaths(watchedDirectories);
    }

    QStringList paths{rootPath};
    if (recursive) {
        QDirIterator iterator(rootPath, QDir::Dirs | QDir::NoDotAndDotDot,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            paths.append(iterator.next());
        }
    }
    // Watch limits are a real constraint: whatever could not be added simply
    // means the manual Refresh and window-activation refresh carry more weight.
    watcher_.addPaths(paths);
}

void DirectoryScanner::setWatchEnabled(bool enabled) {
    watchEnabled_ = enabled;
    if (!enabled) {
        debounce_.stop();
        if (const QStringList directories = watcher_.directories(); !directories.isEmpty()) {
            watcher_.removePaths(directories);
        }
        return;
    }
    if (!watchedRoot_.isEmpty()) {
        rewatch(watchedRoot_, watchRecursive_);
    }
}

void DirectoryScanner::cancelAll() {
    // Bumping the generation is the cancellation mechanism: in-flight work
    // finishes but its result is dropped on delivery.
    state_->generation.fetchAndAddRelaxed(1);
    debounce_.stop();
}

} // namespace cullfinch::infrastructure

#include "DirectoryScanner.moc"
