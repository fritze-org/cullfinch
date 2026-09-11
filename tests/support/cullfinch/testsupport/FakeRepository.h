// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>

#include <QHash>

namespace cullfinch::testsupport {

/// An in-memory repository with fault injection.
///
/// Lets controller tests exercise the failure paths the design demands: a
/// refused draft write must keep the in-memory draft, and a refused mark write
/// must leave the authoritative marks untouched.
class FakeRepository final : public application::IAssetRepository {
public:
    bool open(QString* error) override;
    void close() override;

    std::optional<domain::CollectionId> ensureCollection(const QString& rootPath, bool recursive,
                                                         QString* error) override;
    [[nodiscard]] std::optional<domain::CollectionId> findCollection(const QString& rootPath,
                                                                     QString* error) const override;
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

    // ---- Fault injection and inspection -----------------------------------
    void failNextSessionSaves(int count) { sessionSaveFailures_ = count; }
    void failNextDispositionWrites(int count) { dispositionFailures_ = count; }
    /// Refuse operation journal writes, starting after `after` successes.
    void failOperationSaves(int after, int count) {
        operationSavesBeforeFailure_ = after;
        operationSaveFailures_ = count;
    }
    void setAssets(const domain::CollectionId& id, const domain::PhotoAssetList& assets);
    void setRevision(const domain::CollectionId& id, quint64 revision) {
        revisions_[id] = revision;
    }

    [[nodiscard]] int sessionSaveCount() const { return sessionSaveCount_; }
    [[nodiscard]] int dispositionWriteCount() const { return dispositionWriteCount_; }
    [[nodiscard]] int operationSaveCount() const { return operationSaveCount_; }
    /// Every journal write, in order, so a test can see what a crash at any
    /// point would have left behind.
    [[nodiscard]] const QList<application::OperationRecord>& operationJournal() const {
        return operationJournal_;
    }

private:
    QHash<QString, domain::CollectionId> collectionsByRoot_;
    QHash<domain::CollectionId, quint64> revisions_;
    QHash<domain::CollectionId, domain::PhotoAssetList> assets_;
    QHash<domain::SessionId, application::StoredSession> sessions_;
    QHash<domain::OperationId, application::OperationRecord> operations_;

    int sessionSaveFailures_ = 0;
    int dispositionFailures_ = 0;
    int operationSavesBeforeFailure_ = 0;
    int operationSaveFailures_ = 0;
    int sessionSaveCount_ = 0;
    int dispositionWriteCount_ = 0;
    int operationSaveCount_ = 0;
    QList<application::OperationRecord> operationJournal_;
};

} // namespace cullfinch::testsupport
