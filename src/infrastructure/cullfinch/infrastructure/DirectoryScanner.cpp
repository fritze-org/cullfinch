// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/DirectoryScanner.h>

#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace cullfinch::infrastructure {
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

DirectoryScanner::DirectoryScanner(QObject* parent) : application::IScanService(parent) {
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
}

DirectoryScanner::~DirectoryScanner() {
    cancelAll();

    // Bumping the generation cancels a *result*, not the task that produces it:
    // a worker still dereferences this object to discover that its result is
    // unwanted. So no task may outlive the scanner. Only destruction waits;
    // cancelAll() stays non-blocking, because the GUI thread calls it whenever
    // the user switches collections.
    QList<QFuture<void>> pending;
    {
        const QMutexLocker locked(&inFlightGuard_);
        pending = std::move(inFlight_);
    }
    for (QFuture<void>& task : pending) {
        task.waitForFinished();
    }
}

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
    currentGeneration_.storeRelaxed(request.generation);
    rewatch(request.rootPath, request.recursive);

    const domain::StemAssociationResolver* resolver = &resolver_;
    const quint64 generation = request.generation;
    const QString rootPath = request.rootPath;
    const bool recursive = request.recursive;
    const domain::AssociationConfig config = request.config;
    const domain::CollectionId collectionId = request.collectionId;

    // Decode-free work, so QThreadPool is enough; the scanner never holds a
    // widget pointer.
    QFuture<void> task =
        QtConcurrent::run(QThreadPool::globalInstance(), [this, resolver, generation, rootPath,
                                                          recursive, config, collectionId]() {
            QString error;
            const QList<domain::DiscoveredFile> files = enumerate(rootPath, recursive, &error);
            if (!error.isEmpty()) {
                publishFailed(generation, error);
                return;
            }

            // Classification happens only once enumeration of the association scope
            // has finished for this generation.
            const domain::AssociationResult result =
                resolver->resolve(collectionId, files, config, /*scopeComplete=*/true);
            publishFinished(generation, result);
        });

    // Kept so the destructor can join it. Superseded entries are dropped here
    // rather than by a timer: a scan that has already delivered is nothing to
    // wait for, and a long session must not accumulate them.
    const QMutexLocker locked(&inFlightGuard_);
    inFlight_.removeIf([](const QFuture<void>& pending) { return pending.isFinished(); });
    inFlight_.append(task);
}

void DirectoryScanner::publishFinished(quint64 generation,
                                       const domain::AssociationResult& result) {
    if (currentGeneration_.loadRelaxed() != generation) {
        return; // Superseded before delivery.
    }
    QMetaObject::invokeMethod(
        this,
        [this, generation, result]() {
            if (currentGeneration_.loadRelaxed() != generation) {
                return;
            }
            Q_EMIT scanFinished(generation, result);
        },
        Qt::QueuedConnection);
}

void DirectoryScanner::publishFailed(quint64 generation, const QString& message) {
    QMetaObject::invokeMethod(
        this,
        [this, generation, message]() {
            if (currentGeneration_.loadRelaxed() != generation) {
                return;
            }
            Q_EMIT scanFailed(generation, message);
        },
        Qt::QueuedConnection);
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
    currentGeneration_.fetchAndAddRelaxed(1);
    debounce_.stop();
}

} // namespace cullfinch::infrastructure
