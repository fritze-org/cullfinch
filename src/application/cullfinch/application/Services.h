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

#include <functional>
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

/// The durable steps a member of a file operation passes through.
///
/// Recovery has to reason about these outside the executor -- what a person may
/// be offered about a stopped operation follows from them -- so the vocabulary
/// is named once, here, rather than spelled as a literal at each site.
namespace operationStep {
/// Planned, and where a restore puts the member back to: its original path.
inline constexpr QLatin1String planned{"planned"};
/// Moved into the group's staging directory, verified to have arrived.
inline constexpr QLatin1String staged{"staged"};
/// Put back at its original path by a restore.
inline constexpr QLatin1String restored{"restored"};
/// Known to have reached Trash inside its group directory.
inline constexpr QLatin1String trashed{"trashed"};
/// In neither its original nor its staging location, with nothing on disk able
/// to say where it went. A step of its own rather than a flavour of "staged",
/// because no amount of looking settles it: only a person who has checked the
/// Trash can, and nothing is ever deleted again on a guess.
inline constexpr QLatin1String uncertain{"uncertain"};
} // namespace operationStep

/// One member's durable progress through a file operation.
struct OperationMemberRecord {
    domain::MemberId memberId;
    domain::AssetId assetId;
    QString sourcePath;
    QString stagingPath;
    QString lastDurableStep; ///< One of `operationStep`.
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

/// What a stopped operation's journal allows a person to do about it.
///
/// Derived from the record alone, so the browser can decide what to offer
/// without touching the filesystem. Every offer is still revalidated against
/// disk by the executor when it is taken up: this says what is worth showing,
/// never what is safe to do.
struct RecoveryOffers {
    bool restore = false;        ///< A complete group is in staging and can go back.
    bool retryTrash = false;     ///< Nothing is unaccounted for; Trash can be asked again.
    bool confirmTrashed = false; ///< Only a person can settle where the files went.

    [[nodiscard]] bool any() const { return restore || retryTrash || confirmTrashed; }
};

[[nodiscard]] RecoveryOffers recoveryOffersFor(const OperationRecord& record);

/// True when a record describes work somebody still has to settle.
///
/// Narrower than "not completed": an operation that stopped having left nothing
/// on the source filesystem is over, and so is one whose files are all back
/// where they came from. Showing those as recovery tasks would mean a prompt
/// that never goes away and has nothing to offer.
[[nodiscard]] bool needsRecoveryAttention(const OperationRecord& record);

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

    /// Find the collection for a root directory without creating or touching
    /// it. A read-only instance uses this: it must write nothing.
    [[nodiscard]] virtual std::optional<domain::CollectionId>
    findCollection(const QString& rootPath, QString* error) const = 0;
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

/// Makes an operation record durable. The executor calls it before and after
/// every step that moves a file, so an interruption at any point leaves a
/// journal that says what was intended and what is known to have happened.
/// @return false when the record could not be written; the run then stops.
using JournalWriter = std::function<bool(const OperationRecord& record, QString* error)>;

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
    ///
    /// `journal` is invoked with the record as each step is about to happen
    /// and again once it has; a journal write that fails stops the group.
    virtual OperationRecord executeGroup(const OperationRecord& record,
                                         const domain::PlannedGroup& group,
                                         const JournalWriter& journal) = 0;

    /// Reconcile a journal after a crash or an interrupted run, putting back
    /// everything that is still in staging.
    virtual OperationRecord recover(const OperationRecord& record) = 0;

    /// Ask Trash again for the groups that are still complete in staging.
    ///
    /// The other half of the offer the policy makes when every member staged
    /// but Trash refused. Preconditions are revalidated: a plan with a group
    /// left at its original paths is a restore-and-review job, and nothing is
    /// handed to Trash on the strength of the journal alone.
    virtual OperationRecord retryTrash(const OperationRecord& record) = 0;

    /// Record a person's own confirmation that the groups whose outcome could
    /// not be determined are in Trash.
    ///
    /// Moves nothing and deletes nothing. It answers the one question the
    /// filesystem cannot -- where a group that vanished whole went -- and then
    /// reconciles the journal as recovery would.
    virtual OperationRecord confirmTrashed(const OperationRecord& record) = 0;
};

} // namespace cullfinch::application
