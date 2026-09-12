// SPDX-License-Identifier: GPL-3.0-or-later
//
// A file operation that can be killed at a chosen point, for the crash-injection suite.
//
// The in-process recovery tests capture journal snapshots and feed them back to `recover()`. That
// proves the state machine and nothing about what an *actual* process death leaves behind: whether
// the SQLite journal committed what it claimed, and whether the manifest beside the files survived
// the rename it was meant to explain. Only a second process can show that, and only if it dies the
// way a crash does.
//
// So this helper runs the production `OperationController` over the production `StagingExecutor`
// and a real SQLite database, and calls `_exit()` -- no destructors, no atexit handlers, no
// buffered output -- at the journal write named by `CULLFINCH_CRASH_AT`. The parent suite then
// opens the same database and recovers from whatever is genuinely on disk.
#include <cullfinch/application/OperationController.h>
#include <cullfinch/application/Services.h>
#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/domain/OperationPlan.h>
#include <cullfinch/infrastructure/DirectoryScanner.h>
#include <cullfinch/infrastructure/SqliteRepository.h>
#include <cullfinch/infrastructure/StagingExecutor.h>
#include <cullfinch/testsupport/CrashPoints.h>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QTextStream>

#include <unistd.h>

#include <optional>
#include <utility>

using namespace cullfinch;

namespace {

constexpr int kFailureExitCode = 1;
constexpr int kUsageExitCode = 2;

using testsupport::CrashPoint;

bool hasStep(const application::OperationRecord& record, QLatin1String step) {
    for (const application::OperationMemberRecord& member : record.members) {
        if (member.lastDurableStep == step) {
            return true;
        }
    }
    return false;
}

/// Process death as abrupt as a test can arrange: no unwinding, no destructors, no stdio flush.
/// Anything recovery is going to need has to be on the filesystem already.
[[noreturn]] void die() {
    ::_exit(testsupport::kCrashExitCode);
}

/// An out-parameter the caller is allowed to omit.
void report(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
}

/// Diagnostics for the parent process, which reports them when a run fails.
void reportError(const QString& message) {
    QTextStream stream(stderr);
    stream << QStringLiteral("cullfinch-crash-helper: ") << message << Qt::endl;
}

/// A Trash that is a directory on the same filesystem, so the group's move out of staging is a
/// plain rename and its manifest can be read back afterwards.
///
/// The real adapter would hand the group to the desktop Trash, which the test suite must never
/// touch -- and which would also make "is it still there?" unanswerable.
class DirectoryTrashAdapter final : public application::ITrashAdapter {
public:
    explicit DirectoryTrashAdapter(QString root) : root_(std::move(root)) {}

    bool moveToTrash(const QString& path, QString* resultingPath, QString* error) override {
        if (!QDir().mkpath(root_)) {
            report(error,
                   QStringLiteral("the trash directory '%1' could not be created").arg(root_));
            return false;
        }
        const QString destination = QDir(root_).absoluteFilePath(QFileInfo(path).fileName());
        if (!QDir().rename(path, destination)) {
            report(error, QStringLiteral("'%1' could not be moved to '%2'").arg(path, destination));
            return false;
        }
        if (resultingPath != nullptr) {
            *resultingPath = destination;
        }
        return true;
    }

private:
    QString root_;
};

/// The production repository with one journal write replaced by process death.
///
/// The hook sits at the repository, not inside the executor: the executor and its journalling are
/// what the suite is testing, and a kill switch built into them would only prove itself. Every
/// write the controller and the executor make passes through here, so the process can be stopped
/// exactly between two durable states.
class CrashingRepository final : public application::IAssetRepository {
public:
    CrashingRepository(application::IAssetRepository& inner, CrashPoint point)
        : inner_(inner), point_(point) {}

    bool saveOperation(const application::OperationRecord& record, QString* error) override {
        // Dying before the write leaves the journal one step behind the filesystem: the rename, or
        // the Trash, already happened and nothing recorded it.
        if (diesBefore(record)) {
            die();
        }
        const bool written = inner_.saveOperation(record, error);
        // Dying after it leaves the journal exactly at that step, with the next filesystem
        // operation never attempted.
        if (written && diesAfter(record)) {
            die();
        }
        return written;
    }

    bool open(QString* error) override { return inner_.open(error); }
    void close() override { inner_.close(); }

    bool runInTransaction(const std::function<bool()>& action, QString* error) override {
        return inner_.runInTransaction(action, error);
    }

    std::optional<domain::CollectionId> ensureCollection(const QString& rootPath, bool recursive,
                                                         QString* error) override {
        return inner_.ensureCollection(rootPath, recursive, error);
    }

    [[nodiscard]] std::optional<domain::CollectionId>
    findCollection(const QString& rootPath, QString* error) const override {
        return inner_.findCollection(rootPath, error);
    }

    [[nodiscard]] quint64 collectionRevision(const domain::CollectionId& id,
                                             QString* error) const override {
        return inner_.collectionRevision(id, error);
    }

    bool reconcileAssets(const domain::CollectionId& id, const domain::PhotoAssetList& scanned,
                         domain::PhotoAssetList* merged, quint64* newRevision,
                         QString* error) override {
        return inner_.reconcileAssets(id, scanned, merged, newRevision, error);
    }

    [[nodiscard]] domain::PhotoAssetList loadAssets(const domain::CollectionId& id,
                                                    QString* error) const override {
        return inner_.loadAssets(id, error);
    }

    bool applyDispositions(const domain::CollectionId& id, quint64 expectedRevision,
                           const QList<domain::AssetId>& reject,
                           const QList<domain::AssetId>& neutral, quint64* newRevision,
                           QString* error) override {
        return inner_.applyDispositions(id, expectedRevision, reject, neutral, newRevision, error);
    }

    bool saveSession(const application::StoredSession& session, QString* error) override {
        return inner_.saveSession(session, error);
    }

    [[nodiscard]] std::optional<application::StoredSession>
    loadSession(const domain::SessionId& id, QString* error) const override {
        return inner_.loadSession(id, error);
    }

    [[nodiscard]] QList<application::StoredSession>
    resumableSessions(const domain::CollectionId& id, QString* error) const override {
        return inner_.resumableSessions(id, error);
    }

    bool deleteSession(const domain::SessionId& id, QString* error) override {
        return inner_.deleteSession(id, error);
    }

    [[nodiscard]] std::optional<application::OperationRecord>
    loadOperation(const domain::OperationId& id, QString* error) const override {
        return inner_.loadOperation(id, error);
    }

    [[nodiscard]] QList<application::OperationRecord>
    unfinishedOperations(const domain::CollectionId& id, QString* error) const override {
        return inner_.unfinishedOperations(id, error);
    }

private:
    [[nodiscard]] bool diesBefore(const application::OperationRecord& record) const {
        switch (point_) {
        case CrashPoint::FirstRename:
            // The first write that would confirm a rename. The file is in staging already; this is
            // the write that was going to say so.
            return record.state == domain::OperationState::Staging &&
                   hasStep(record, QLatin1String("staged"));
        case CrashPoint::Trashed:
            // Trash has returned. This is the write that was going to record the outcome and the
            // Trash path, so without it the journal still reads "Trashing" with no path.
            return record.state == domain::OperationState::Trashing &&
                   hasStep(record, QLatin1String("trashed"));
        case CrashPoint::None:
        case CrashPoint::Staged:
        case CrashPoint::Trashing:
            return false;
        }
        return false;
    }

    [[nodiscard]] bool diesAfter(const application::OperationRecord& record) const {
        switch (point_) {
        case CrashPoint::Staged:
            // Every file is in staging and verified; Trash has not been mentioned yet.
            return record.state == domain::OperationState::Staged;
        case CrashPoint::Trashing:
            // The intent to Trash is durable and Trash has not been called: recovery must not take
            // "Trashing" as evidence that it was.
            return record.state == domain::OperationState::Trashing &&
                   !hasStep(record, QLatin1String("trashed"));
        case CrashPoint::None:
        case CrashPoint::FirstRename:
        case CrashPoint::Trashed:
            return false;
        }
        return false;
    }

    application::IAssetRepository& inner_;
    CrashPoint point_;
};

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("cullfinch-crash-helper"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Runs a reviewed cullfinch file operation and dies at the journal write "
                       "named by CULLFINCH_CRASH_AT."));
    parser.addHelpOption();
    const QCommandLineOption collectionOption(QStringLiteral("collection"),
                                              QStringLiteral("Collection root to operate on."),
                                              QStringLiteral("directory"));
    const QCommandLineOption stagingOption(QStringLiteral("staging"),
                                           QStringLiteral("Staging root, on the same filesystem."),
                                           QStringLiteral("directory"));
    const QCommandLineOption databaseOption(QStringLiteral("database"),
                                            QStringLiteral("SQLite journal to write."),
                                            QStringLiteral("file"));
    const QCommandLineOption trashOption(QStringLiteral("trash"),
                                         QStringLiteral("Directory standing in for the Trash."),
                                         QStringLiteral("directory"));
    parser.addOption(collectionOption);
    parser.addOption(stagingOption);
    parser.addOption(databaseOption);
    parser.addOption(trashOption);
    parser.process(app);

    const QString collectionRoot = parser.value(collectionOption);
    const QString stagingRoot = parser.value(stagingOption);
    const QString databaseFile = parser.value(databaseOption);
    const QString trashRoot = parser.value(trashOption);
    if (collectionRoot.isEmpty() || stagingRoot.isEmpty() || databaseFile.isEmpty() ||
        trashRoot.isEmpty()) {
        reportError(
            QStringLiteral("--collection, --staging, --database and --trash are required."));
        return kUsageExitCode;
    }

    // An unset variable means "do not crash", so a plain run needs no special casing. Anything else
    // must name a step this build knows: a typo that quietly ran the operation to completion would
    // leave the suite asserting a crash that never happened.
    const QString requested = QString::fromUtf8(qgetenv("CULLFINCH_CRASH_AT"));
    const std::optional<CrashPoint> point =
        requested.isEmpty() ? CrashPoint::None : testsupport::crashPointFromToken(requested);
    if (!point.has_value()) {
        reportError(QStringLiteral("CULLFINCH_CRASH_AT='%1' names no known step.").arg(requested));
        return kUsageExitCode;
    }

    infrastructure::SqliteRepository repository(databaseFile);
    QString error;
    if (!repository.open(&error)) {
        reportError(error);
        return kFailureExitCode;
    }

    const std::optional<domain::CollectionId> collectionId =
        repository.ensureCollection(collectionRoot, false, &error);
    if (!collectionId.has_value()) {
        reportError(error);
        return kFailureExitCode;
    }

    const QList<domain::DiscoveredFile> files =
        infrastructure::DirectoryScanner::enumerate(collectionRoot, false, &error);
    if (files.isEmpty()) {
        reportError(QStringLiteral("'%1' holds no files: %2").arg(collectionRoot, error));
        return kFailureExitCode;
    }

    // Everything in the collection is marked for deletion: what the helper runs is decided by what
    // the parent put in the directory, not by anything in here.
    const domain::StemAssociationResolver resolver;
    domain::PhotoAssetList assets =
        resolver.resolve(*collectionId, files, domain::AssociationConfig::defaults(), true).assets;
    for (domain::PhotoAsset& asset : assets) {
        asset.disposition = domain::Disposition::Reject;
    }

    DirectoryTrashAdapter trash(trashRoot);
    infrastructure::StagingExecutor executor(trash);
    CrashingRepository journal(repository, *point);
    application::OperationController controller(journal, executor);

    const domain::PlanningResult planning =
        controller.review(*collectionId, 1, stagingRoot, assets);
    if (planning.plan.isEmpty()) {
        reportError(QStringLiteral("nothing was planned for '%1'.").arg(collectionRoot));
        return kFailureExitCode;
    }

    // The parent recovers by operation identifier, so it has to have the identifier before the
    // crash takes it: _exit() flushes nothing.
    QTextStream out(stdout);
    out << QStringLiteral("operation ") << planning.plan.id.toString() << Qt::endl;

    if (!controller.execute(planning.plan, assets, &error)) {
        reportError(error);
        return kFailureExitCode;
    }
    return 0;
}
