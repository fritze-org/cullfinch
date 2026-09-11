// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/domain/AssociationPolicy.h>
#include <cullfinch/domain/FlowContract.h>
#include <cullfinch/domain/OperationPlan.h>
#include <cullfinch/domain/PhotoAsset.h>
#include <cullfinch/domain/SelectionSnapshot.h>

#include <QDateTime>
#include <QList>
#include <QString>

#include <optional>

namespace cullfinch::application {

/// Where a comparison session is in its lifecycle.
enum class SessionLifecycle { Active, Paused, Finished, Discarded };

[[nodiscard]] QString sessionLifecycleToken(SessionLifecycle lifecycle);
[[nodiscard]] SessionLifecycle sessionLifecycleFromToken(const QString& token);

/// A session as persisted. `draft` carries the flow's own opaque payload, which
/// the host stores and versions but never interprets.
struct StoredSession {
    domain::SessionId id;
    domain::CollectionId collectionId;
    domain::SelectionSnapshot snapshot;
    domain::VersionedFlowState draft;
    QList<domain::AssetId> draftRejected;
    SessionLifecycle lifecycle = SessionLifecycle::Active;
    QDateTime createdUtc;
    QDateTime updatedUtc;
};

/// One member's durable progress through a file operation.
struct OperationMemberRecord {
    domain::MemberId memberId;
    domain::AssetId assetId;
    QString sourcePath;
    QString stagingPath;
    QString lastDurableStep; ///< "planned", "staged", "restored", ...
    QString error;
};

/// The persisted view of an operation, including its journal.
struct OperationRecord {
    domain::OperationPlan plan;
    domain::OperationState state = domain::OperationState::Planned;
    QList<OperationMemberRecord> members;
    QString trashPath; ///< May be empty: the platform need not report one.
    QString error;
    QDateTime createdUtc;
    QDateTime updatedUtc;
};

/// Metadata persistence. Owned and used on its assigned thread; operation work
/// is serialised independently of thumbnail work.
class IAssetRepository {
public:
    IAssetRepository() = default;
    virtual ~IAssetRepository() = default;
    IAssetRepository(const IAssetRepository&) = delete;
    IAssetRepository& operator=(const IAssetRepository&) = delete;
    IAssetRepository(IAssetRepository&&) = delete;
    IAssetRepository& operator=(IAssetRepository&&) = delete;

    virtual bool open(QString* error) = 0;
    virtual void close() = 0;

    /// Create or find the collection for a root directory.
    virtual std::optional<domain::CollectionId>
    ensureCollection(const QString& rootPath, bool recursive, QString* error) = 0;
    [[nodiscard]] virtual quint64 collectionRevision(const domain::CollectionId& id,
                                                     QString* error) const = 0;

    /// Replace the known asset set after a scan, reconciling stored identities
    /// and preserving deletion marks for assets whose membership is unchanged.
    virtual bool reconcileAssets(const domain::CollectionId& id,
                                 const domain::PhotoAssetList& scanned,
                                 domain::PhotoAssetList* merged, quint64* newRevision,
                                 QString* error) = 0;

    [[nodiscard]] virtual domain::PhotoAssetList loadAssets(const domain::CollectionId& id,
                                                            QString* error) const = 0;

    /// Apply deletion marks in one transaction, guarded by the collection
    /// revision the caller believes it is working against.
    virtual bool applyDispositions(const domain::CollectionId& id, quint64 expectedRevision,
                                   const QList<domain::AssetId>& reject,
                                   const QList<domain::AssetId>& neutral, quint64* newRevision,
                                   QString* error) = 0;

    virtual bool saveSession(const StoredSession& session, QString* error) = 0;
    [[nodiscard]] virtual std::optional<StoredSession> loadSession(const domain::SessionId& id,
                                                                   QString* error) const = 0;
    [[nodiscard]] virtual QList<StoredSession> resumableSessions(const domain::CollectionId& id,
                                                                 QString* error) const = 0;
    virtual bool deleteSession(const domain::SessionId& id, QString* error) = 0;

    virtual bool saveOperation(const OperationRecord& record, QString* error) = 0;
    [[nodiscard]] virtual std::optional<OperationRecord>
    loadOperation(const domain::OperationId& id, QString* error) const = 0;
    [[nodiscard]] virtual QList<OperationRecord>
    unfinishedOperations(const domain::CollectionId& id, QString* error) const = 0;
};

/// Guards a collection against simultaneous cullfinch *writers*.
///
/// A second instance that cannot take the lock may still open the collection
/// read-only. External tools are outside this lock entirely, which is exactly
/// why every file operation revalidates its preconditions immediately before
/// execution. Implemented in the infrastructure layer.
class ICollectionLock {
public:
    ICollectionLock() = default;
    virtual ~ICollectionLock() = default;
    ICollectionLock(const ICollectionLock&) = delete;
    ICollectionLock& operator=(const ICollectionLock&) = delete;
    ICollectionLock(ICollectionLock&&) = delete;
    ICollectionLock& operator=(ICollectionLock&&) = delete;

    /// Take the writer lock for `collectionRoot`, releasing any other root
    /// this lock currently holds.
    ///
    /// @param holder on false, describes the process that holds the lock,
    ///        where the platform reports it.
    virtual bool acquire(const QString& collectionRoot, QString* holder) = 0;
    virtual void release() = 0;
    [[nodiscard]] virtual bool isHeld() const = 0;
};

/// Moving a group to Trash. Behind an adapter so GUI tests can use a fake one
/// and never touch the user's real Trash.
class ITrashAdapter {
public:
    ITrashAdapter() = default;
    virtual ~ITrashAdapter() = default;
    ITrashAdapter(const ITrashAdapter&) = delete;
    ITrashAdapter& operator=(const ITrashAdapter&) = delete;
    ITrashAdapter(ITrashAdapter&&) = delete;
    ITrashAdapter& operator=(ITrashAdapter&&) = delete;

    /// Move `path` to Trash.
    ///
    /// @param resultingPath set when the platform reports one; it legitimately
    ///        stays empty otherwise.
    /// @return false on failure. Never falls back to permanent deletion.
    virtual bool moveToTrash(const QString& path, QString* resultingPath, QString* error) = 0;
};

/// Executes a reviewed plan: recoverable same-filesystem staging, then one
/// Trash call on the completed group directory.
class IOperationExecutor {
public:
    IOperationExecutor() = default;
    virtual ~IOperationExecutor() = default;
    IOperationExecutor(const IOperationExecutor&) = delete;
    IOperationExecutor& operator=(const IOperationExecutor&) = delete;
    IOperationExecutor(IOperationExecutor&&) = delete;
    IOperationExecutor& operator=(IOperationExecutor&&) = delete;

    /// Re-enumerate and verify the plan immediately before execution.
    ///
    /// If anything differs from what was reviewed, the plan is invalidated and
    /// the caller must return to review.
    [[nodiscard]] virtual domain::PlanningResult preflight(const domain::OperationPlan& plan,
                                                           const domain::PhotoAssetList& current,
                                                           QString* error) const = 0;

    /// Run one group to completion or to a recorded recoverable state.
    virtual OperationRecord executeGroup(const OperationRecord& record,
                                         const domain::PlannedGroup& group) = 0;

    /// Reconcile a journal after a crash or an interrupted run.
    virtual OperationRecord recover(const OperationRecord& record) = 0;
};

} // namespace cullfinch::application
