// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/testsupport/FakeRepository.h>

#include <QCryptographicHash>

namespace cullfinch::testsupport {
namespace {

void report(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

bool FakeRepository::open(QString* error) {
    Q_UNUSED(error)
    return true;
}

void FakeRepository::close() {
    // Nothing to release: the fake keeps its rows in memory for the lifetime of
    // the instance, so a test can inspect them after closing.
}

bool FakeRepository::runInTransaction(const std::function<bool()>& action, QString* error) {
    Q_UNUSED(error)
    // No real transaction exists here, only a snapshot of the rows a nested
    // write can touch. Only the outermost call takes it and, on failure,
    // restores it -- exactly what a real rollback would leave behind -- so a
    // caller composing several fake writes (see SessionController::finish)
    // sees the same all-or-nothing behaviour a real repository gives it.
    const bool outermost = transactionDepth_ == 0;
    const QHash<domain::CollectionId, quint64> revisionsSnapshot = revisions_;
    const QHash<domain::CollectionId, domain::PhotoAssetList> assetsSnapshot = assets_;
    const QHash<domain::SessionId, application::StoredSession> sessionsSnapshot = sessions_;

    ++transactionDepth_;
    const bool ok = action();
    --transactionDepth_;

    if (!ok && outermost) {
        revisions_ = revisionsSnapshot;
        assets_ = assetsSnapshot;
        sessions_ = sessionsSnapshot;
    }
    return ok;
}

std::optional<domain::CollectionId>
FakeRepository::ensureCollection(const QString& rootPath, bool recursive, QString* error) {
    Q_UNUSED(recursive)
    Q_UNUSED(error)
    const auto existing = collectionsByRoot_.constFind(rootPath);
    if (existing != collectionsByRoot_.constEnd()) {
        return *existing;
    }
    domain::CollectionId id(QString::fromLatin1(
        QCryptographicHash::hash(rootPath.toUtf8(), QCryptographicHash::Sha1).toHex()));
    collectionsByRoot_.insert(rootPath, id);
    revisions_.insert(id, 1);
    return id;
}

std::optional<domain::CollectionId> FakeRepository::findCollection(const QString& rootPath,
                                                                   QString* error) const {
    const auto existing = collectionsByRoot_.constFind(rootPath);
    if (existing == collectionsByRoot_.constEnd()) {
        report(error, QStringLiteral("no such collection"));
        return std::nullopt;
    }
    return *existing;
}

quint64 FakeRepository::collectionRevision(const domain::CollectionId& id, QString* error) const {
    Q_UNUSED(error)
    return revisions_.value(id, 0);
}

void FakeRepository::setAssets(const domain::CollectionId& id,
                               const domain::PhotoAssetList& assets) {
    assets_.insert(id, assets);
    revisions_[id] = revisions_.value(id, 0) + 1;
}

bool FakeRepository::reconcileAssets(const domain::CollectionId& id,
                                     const domain::PhotoAssetList& scanned,
                                     domain::PhotoAssetList* merged, quint64* newRevision,
                                     QString* error) {
    Q_UNUSED(error)
    QHash<domain::AssetId, domain::Disposition> previous;
    for (const domain::PhotoAsset& asset : assets_.value(id)) {
        previous.insert(asset.id, asset.disposition);
    }

    domain::PhotoAssetList result = scanned;
    for (domain::PhotoAsset& asset : result) {
        asset.disposition = previous.value(asset.id, domain::Disposition::Neutral);
    }
    assets_.insert(id, result);
    revisions_[id] = revisions_.value(id, 0) + 1;

    if (merged != nullptr) {
        *merged = result;
    }
    if (newRevision != nullptr) {
        *newRevision = revisions_.value(id);
    }
    return true;
}

domain::PhotoAssetList FakeRepository::loadAssets(const domain::CollectionId& id,
                                                  QString* error) const {
    Q_UNUSED(error)
    return assets_.value(id);
}

bool FakeRepository::applyDispositions(const domain::CollectionId& id, quint64 expectedRevision,
                                       const QList<domain::AssetId>& reject,
                                       const QList<domain::AssetId>& neutral, quint64* newRevision,
                                       QString* error) {
    ++dispositionWriteCount_;
    if (dispositionFailures_ > 0) {
        --dispositionFailures_;
        report(error, QStringLiteral("fake storage failure"));
        return false;
    }
    if (revisions_.value(id, 0) != expectedRevision) {
        report(error, QStringLiteral("collection revision mismatch"));
        return false;
    }

    domain::PhotoAssetList assets = assets_.value(id);
    for (domain::PhotoAsset& asset : assets) {
        if (reject.contains(asset.id)) {
            asset.disposition = domain::Disposition::Reject;
        } else if (neutral.contains(asset.id)) {
            asset.disposition = domain::Disposition::Neutral;
        }
    }
    assets_.insert(id, assets);
    revisions_[id] = expectedRevision + 1;
    if (newRevision != nullptr) {
        *newRevision = revisions_.value(id);
    }
    return true;
}

bool FakeRepository::saveSession(const application::StoredSession& session, QString* error) {
    ++sessionSaveCount_;
    if (sessionSaveFailures_ > 0) {
        if (sessionSaveSuccessesBeforeFailure_ > 0) {
            --sessionSaveSuccessesBeforeFailure_;
        } else {
            --sessionSaveFailures_;
            report(error, QStringLiteral("fake draft write failure"));
            return false;
        }
    }
    sessions_.insert(session.id, session);
    return true;
}

std::optional<application::StoredSession> FakeRepository::loadSession(const domain::SessionId& id,
                                                                      QString* error) const {
    const auto found = sessions_.constFind(id);
    if (found == sessions_.constEnd()) {
        report(error, QStringLiteral("no such session"));
        return std::nullopt;
    }
    return *found;
}

QList<application::StoredSession> FakeRepository::resumableSessions(const domain::CollectionId& id,
                                                                    QString* error) const {
    Q_UNUSED(error)
    QList<application::StoredSession> result;
    for (const application::StoredSession& session : sessions_) {
        if (session.collectionId == id &&
            (session.lifecycle == application::SessionLifecycle::Active ||
             session.lifecycle == application::SessionLifecycle::Paused)) {
            result.append(session);
        }
    }
    return result;
}

bool FakeRepository::deleteSession(const domain::SessionId& id, QString* error) {
    Q_UNUSED(error)
    sessions_.remove(id);
    return true;
}

bool FakeRepository::saveOperation(const application::OperationRecord& record, QString* error) {
    if (operationSaveFailures_ > 0) {
        if (operationSavesBeforeFailure_ > 0) {
            --operationSavesBeforeFailure_;
        } else {
            --operationSaveFailures_;
            report(error, QStringLiteral("fake journal write failure"));
            return false;
        }
    }
    ++operationSaveCount_;
    operationJournal_.append(record);
    operations_.insert(record.plan.id, record);
    return true;
}

std::optional<application::OperationRecord>
FakeRepository::loadOperation(const domain::OperationId& id, QString* error) const {
    const auto found = operations_.constFind(id);
    if (found == operations_.constEnd()) {
        report(error, QStringLiteral("no such operation"));
        return std::nullopt;
    }
    return *found;
}

QList<application::OperationRecord>
FakeRepository::unfinishedOperations(const domain::CollectionId& id, QString* error) const {
    Q_UNUSED(error)
    QList<application::OperationRecord> result;
    for (const application::OperationRecord& record : operations_) {
        if (record.plan.collectionId == id && record.state != domain::OperationState::Completed) {
            result.append(record);
        }
    }
    return result;
}

} // namespace cullfinch::testsupport
