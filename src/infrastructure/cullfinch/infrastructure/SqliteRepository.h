// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>

#include <QSqlDatabase>
#include <QString>

namespace cullfinch::infrastructure {

/// Application metadata in a local SQLite database.
///
/// The database lives under the platform application-data location, never in
/// the collection: marking and comparing must not require write access to the
/// photos, and the live database must not sit on a network share.
///
/// The connection is owned and used on the thread that opened it. Operation
/// work is serialised independently of thumbnail work by using separate
/// repository instances with their own connections.
class SqliteRepository final : public application::IAssetRepository {
public:
    explicit SqliteRepository(QString databaseFile, QString connectionName = QString());
    ~SqliteRepository() override;

    SqliteRepository(const SqliteRepository&) = delete;
    SqliteRepository& operator=(const SqliteRepository&) = delete;
    SqliteRepository(SqliteRepository&&) = delete;
    SqliteRepository& operator=(SqliteRepository&&) = delete;

    bool open(QString* error) override;
    void close() override;

    bool runInTransaction(const std::function<bool()>& action, QString* error) override;

    [[nodiscard]] std::optional<domain::CollectionId> findCollection(const QString& rootPath,
                                                                     QString* error) const override;
    std::optional<domain::CollectionId> ensureCollection(const QString& rootPath, bool recursive,
                                                         QString* error) override;
    [[nodiscard]] quint64 collectionRevision(const domain::CollectionId& id,
                                             QString* error) const override;

    bool reconcileAssets(const domain::CollectionId& id, const domain::PhotoAssetList& scanned,
                         domain::PhotoAssetList* merged, quint64* newRevision,
                         QString* error) override;

    [[nodiscard]] domain::PhotoAssetList loadAssets(const domain::CollectionId& id,
                                                    QString* error) const override;

    bool applyDispositions(const domain::CollectionId& id, quint64 expectedRevision,
                           const QList<domain::AssetId>& reject,
                           const QList<domain::AssetId>& neutral, quint64* newRevision,
                           QString* error) override;

    bool saveSession(const application::StoredSession& session, QString* error) override;
    [[nodiscard]] std::optional<application::StoredSession>
    loadSession(const domain::SessionId& id, QString* error) const override;
    [[nodiscard]] QList<application::StoredSession>
    resumableSessions(const domain::CollectionId& id, QString* error) const override;
    bool deleteSession(const domain::SessionId& id, QString* error) override;

    bool saveOperation(const application::OperationRecord& record, QString* error) override;
    [[nodiscard]] std::optional<application::OperationRecord>
    loadOperation(const domain::OperationId& id, QString* error) const override;
    [[nodiscard]] QList<application::OperationRecord>
    unfinishedOperations(const domain::CollectionId& id, QString* error) const override;

    /// The schema version this build expects.
    [[nodiscard]] static int targetSchemaVersion();

private:
    bool migrate(QString* error);

    /// Opens the connection's transaction unless one from an enclosing
    /// `runInTransaction` call is already open, in which case this joins it.
    bool beginTransactionScope(QString* error);
    /// Leaves the transaction scope opened by the matching `beginTransactionScope`
    /// call. Only the outermost, unmatched call actually commits or rolls back;
    /// an inner one just reports `commit` up to whichever call is outermost.
    bool endTransactionScope(bool commit, QString* error);

    /// The body of `applyDispositions`, run inside the transaction scope
    /// `runInTransaction` opens -- split out so that scope's lambda stays
    /// short and its captures explicit.
    bool applyDispositionsLocked(const domain::CollectionId& id, quint64 expectedRevision,
                                 const QList<domain::AssetId>& reject,
                                 const QList<domain::AssetId>& neutral, QString* error) const;

    QString databaseFile_;
    QString connectionName_;
    QSqlDatabase database_;
    bool open_ = false;
    int transactionDepth_ = 0;
    /// Set when any scope in the current transaction failed. The outermost
    /// one then rolls back rather than committing writes a failed scope left
    /// behind.
    bool rollbackOnly_ = false;
};

} // namespace cullfinch::infrastructure
