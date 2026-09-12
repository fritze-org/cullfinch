// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/SqliteRepository.h>

#include <cullfinch/domain/AssociationPolicy.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <utility>

namespace cullfinch::infrastructure {
namespace {

using application::OperationMemberRecord;
using application::OperationRecord;
using application::StoredSession;
using domain::AssetId;
using domain::CollectionId;
using domain::Disposition;
using domain::FileMember;
using domain::MemberId;
using domain::MemberRole;
using domain::PairingState;
using domain::PhotoAsset;
using domain::PhotoAssetList;

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

void report(QString* error, const QString& message) {
    if (error != nullptr) {
        *error = message;
    }
}

/// Bind a string that must not become SQL NULL.
///
/// SQLite distinguishes a null QString from an empty one, and every text column
/// in this schema is NOT NULL. A default-constructed QString is null -- which is
/// exactly what an asset sitting directly in the scan root has for its relative
/// directory, and what an operation has for its Trash path until the platform
/// reports one. Binding that directly overrides the column default and the
/// insert fails.
QVariant text(const QString& value) {
    return value.isNull() ? QVariant(QString(QLatin1String(""))) : QVariant(value);
}

bool fail(QString* error, const QSqlQuery& query, const QString& what) {
    report(error, tr("%1 failed: %2").arg(what, query.lastError().text()));
    return false;
}

QString pairingToken(PairingState state) {
    using enum PairingState;
    switch (state) {
    case Provisional:
        return QStringLiteral("provisional");
    case Resolved:
        return QStringLiteral("resolved");
    case JpegOnly:
        return QStringLiteral("jpeg-only");
    case RawOnly:
        return QStringLiteral("raw-only");
    case Ambiguous:
        return QStringLiteral("ambiguous");
    case Stale:
        break;
    }
    return QStringLiteral("stale");
}

PairingState pairingFromToken(const QString& token) {
    using enum PairingState;
    static const QHash<QString, PairingState> states = {
        {QStringLiteral("provisional"), Provisional}, {QStringLiteral("resolved"), Resolved},
        {QStringLiteral("jpeg-only"), JpegOnly},      {QStringLiteral("raw-only"), RawOnly},
        {QStringLiteral("ambiguous"), Ambiguous},     {QStringLiteral("stale"), Stale}};
    // An unreadable value must not become a comparable, operable group.
    return states.value(token, Ambiguous);
}

QString roleToken(MemberRole role) {
    using enum MemberRole;
    switch (role) {
    case Jpeg:
        return QStringLiteral("jpeg");
    case Raw:
        return QStringLiteral("raw");
    case Sidecar:
        return QStringLiteral("sidecar");
    case Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

MemberRole roleFromToken(const QString& token) {
    using enum MemberRole;
    if (token == QLatin1String("jpeg")) {
        return Jpeg;
    }
    if (token == QLatin1String("raw")) {
        return Raw;
    }
    if (token == QLatin1String("sidecar")) {
        return Sidecar;
    }
    return Unknown;
}

/// Re-attach the members a fresh scan no longer sees, and report whether any
/// were missing.
///
/// A member that disappeared after pairing keeps its place and marks the asset
/// stale. It is never silently reclassified as JPG-only.
///
/// Retention is keyed on member identity as well as path: a member row is unique
/// per (id, asset), so re-adding one whose identity a current member already
/// carries would abort the whole scan on a primary-key conflict rather than
/// merely mis-describe one photo.
bool carryForwardVanishedMembers(const PhotoAsset& previous, PhotoAsset& asset) {
    QSet<QString> currentPaths;
    QSet<QString> currentIds;
    for (const FileMember& member : asset.members) {
        currentPaths.insert(member.absolutePath);
        currentIds.insert(member.id.toString());
    }

    bool stale = false;
    for (const FileMember& member : previous.members) {
        if (currentPaths.contains(member.absolutePath) ||
            currentIds.contains(member.id.toString())) {
            continue;
        }
        asset.members.append(member);
        currentIds.insert(member.id.toString());
        asset.diagnostics.append(
            tr("'%1' was part of this photo but is no longer on disk.").arg(member.fileName));
        stale = true;
    }
    return stale;
}

/// Merge a fresh scan onto what storage already holds.
PhotoAssetList mergeWithStored(const PhotoAssetList& scanned, const PhotoAssetList& stored) {
    QHash<AssetId, const PhotoAsset*> storedById;
    for (const PhotoAsset& asset : stored) {
        storedById.insert(asset.id, &asset);
    }

    PhotoAssetList result;
    result.reserve(scanned.size());
    for (const PhotoAsset& fresh : scanned) {
        PhotoAsset asset = fresh;
        if (const PhotoAsset* previous = storedById.value(asset.id, nullptr); previous != nullptr) {
            if (carryForwardVanishedMembers(*previous, asset)) {
                asset.pairingState = PairingState::Stale;
                asset.operationsBlocked = true;
            }

            // Identity can be reconciled only when membership is unchanged. A
            // path replacement invalidates prior decisions rather than
            // inheriting a rejection mark blindly.
            asset.disposition = previous->membershipRevision == fresh.membershipRevision
                                    ? previous->disposition
                                    : Disposition::Neutral;
        }
        result.append(asset);
    }
    return result;
}

void bindAsset(QSqlQuery& query, const CollectionId& collection, const PhotoAsset& asset) {
    query.bindValue(QStringLiteral(":id"), text(asset.id.toString()));
    query.bindValue(QStringLiteral(":collection"), text(collection.toString()));
    query.bindValue(QStringLiteral(":stem"), text(asset.stem));
    query.bindValue(QStringLiteral(":directory"), text(asset.relativeDirectory));
    query.bindValue(QStringLiteral(":display"), text(asset.displayName));
    query.bindValue(QStringLiteral(":preview"), text(asset.previewMemberId.toString()));
    query.bindValue(QStringLiteral(":pairing"), text(pairingToken(asset.pairingState)));
    query.bindValue(QStringLiteral(":disposition"), asset.disposition == Disposition::Reject
                                                        ? QStringLiteral("reject")
                                                        : QStringLiteral("neutral"));
    query.bindValue(QStringLiteral(":revision"), QString::number(asset.membershipRevision));
    query.bindValue(QStringLiteral(":blocked"), asset.operationsBlocked ? 1 : 0);
    query.bindValue(QStringLiteral(":diagnostics"),
                    text(asset.diagnostics.join(QLatin1Char('\n'))));
}

void bindMember(QSqlQuery& query, const CollectionId& collection, const PhotoAsset& asset,
                const FileMember& member, int ordinal) {
    query.bindValue(QStringLiteral(":id"), text(member.id.toString()));
    query.bindValue(QStringLiteral(":asset"), text(asset.id.toString()));
    query.bindValue(QStringLiteral(":collection"), text(collection.toString()));
    query.bindValue(QStringLiteral(":role"), text(roleToken(member.role)));
    query.bindValue(QStringLiteral(":path"), text(member.absolutePath));
    query.bindValue(QStringLiteral(":name"), text(member.fileName));
    query.bindValue(QStringLiteral(":extension"), text(member.extensionLower));
    query.bindValue(QStringLiteral(":size"), static_cast<qlonglong>(member.fingerprint.sizeBytes));
    query.bindValue(QStringLiteral(":modified"),
                    static_cast<qlonglong>(member.fingerprint.modifiedMsecsUtc));
    query.bindValue(QStringLiteral(":device"), QString::number(member.fingerprint.native.device));
    query.bindValue(QStringLiteral(":fileId"), QString::number(member.fingerprint.native.fileId));
    query.bindValue(QStringLiteral(":known"), member.fingerprint.native.known ? 1 : 0);
    query.bindValue(QStringLiteral(":symlink"), member.isSymlink ? 1 : 0);
    query.bindValue(QStringLiteral(":ordinal"), ordinal);
}

/// Bind and run the disposition update for every identifier, stopping at the
/// first failure; the caller reports the query's error.
bool applyDisposition(QSqlQuery& update, const CollectionId& collection, const QList<AssetId>& ids,
                      const QString& token) {
    for (const AssetId& assetId : ids) {
        update.bindValue(QStringLiteral(":disposition"), text(token));
        update.bindValue(QStringLiteral(":id"), text(assetId.toString()));
        update.bindValue(QStringLiteral(":collection"), text(collection.toString()));
        if (!update.exec()) {
            return false;
        }
    }
    return true;
}

QString jsonToString(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonObject jsonFromString(const QString& text) {
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

QJsonObject snapshotToJson(const domain::SelectionSnapshot& snapshot) {
    QJsonObject object;
    object.insert(QLatin1String("collectionId"), snapshot.collectionId.toString());
    object.insert(QLatin1String("collectionRevision"),
                  QString::number(snapshot.collectionRevision));
    object.insert(QLatin1String("order"), domain::toJsonArray(snapshot.orderedAssetIds));

    QJsonObject revisions;
    for (auto it = snapshot.membershipRevisions.cbegin(); it != snapshot.membershipRevisions.cend();
         ++it) {
        revisions.insert(it.key().toString(), QString::number(it.value()));
    }
    object.insert(QLatin1String("membershipRevisions"), revisions);
    return object;
}

domain::SelectionSnapshot snapshotFromJson(const QJsonObject& object) {
    domain::SelectionSnapshot snapshot;
    snapshot.collectionId = CollectionId(object.value(QLatin1String("collectionId")).toString());
    snapshot.collectionRevision =
        object.value(QLatin1String("collectionRevision")).toString().toULongLong();
    snapshot.orderedAssetIds =
        domain::assetIdsFromJson(object.value(QLatin1String("order")).toArray());

    const QJsonObject revisions = object.value(QLatin1String("membershipRevisions")).toObject();
    for (auto it = revisions.constBegin(); it != revisions.constEnd(); ++it) {
        snapshot.membershipRevisions.insert(AssetId(it.key()), it.value().toString().toULongLong());
    }
    return snapshot;
}

QJsonObject planToJson(const domain::OperationPlan& plan) {
    QJsonArray groups;
    for (const domain::PlannedGroup& group : plan.groups) {
        QJsonArray members;
        for (const domain::PlannedMember& member : group.members) {
            QJsonObject entry;
            entry.insert(QLatin1String("memberId"), member.memberId.toString());
            entry.insert(QLatin1String("role"), roleToken(member.role));
            entry.insert(QLatin1String("sourcePath"), member.sourcePath);
            entry.insert(QLatin1String("fileName"), member.fileName);
            entry.insert(QLatin1String("sizeBytes"), QString::number(member.expected.sizeBytes));
            entry.insert(QLatin1String("modifiedMsecs"),
                         QString::number(member.expected.modifiedMsecsUtc));
            entry.insert(QLatin1String("nativeDevice"),
                         QString::number(member.expected.native.device));
            entry.insert(QLatin1String("nativeFileId"),
                         QString::number(member.expected.native.fileId));
            entry.insert(QLatin1String("nativeKnown"), member.expected.native.known);
            members.append(entry);
        }

        QJsonObject entry;
        entry.insert(QLatin1String("assetId"), group.assetId.toString());
        entry.insert(QLatin1String("displayName"), group.displayName);
        entry.insert(QLatin1String("stagingDirectoryName"), group.stagingDirectoryName);
        entry.insert(QLatin1String("members"), members);
        groups.append(entry);
    }

    QJsonObject object;
    object.insert(QLatin1String("id"), plan.id.toString());
    object.insert(QLatin1String("collectionId"), plan.collectionId.toString());
    object.insert(QLatin1String("collectionRevision"), QString::number(plan.collectionRevision));
    object.insert(QLatin1String("stagingRoot"), plan.stagingRoot);
    object.insert(QLatin1String("groups"), groups);
    return object;
}

domain::OperationPlan planFromJson(const QJsonObject& object) {
    domain::OperationPlan plan;
    plan.id = domain::OperationId(object.value(QLatin1String("id")).toString());
    plan.collectionId = CollectionId(object.value(QLatin1String("collectionId")).toString());
    plan.collectionRevision =
        object.value(QLatin1String("collectionRevision")).toString().toULongLong();
    plan.stagingRoot = object.value(QLatin1String("stagingRoot")).toString();

    for (const auto& groupValue : object.value(QLatin1String("groups")).toArray()) {
        const QJsonObject groupObject = groupValue.toObject();
        domain::PlannedGroup group;
        group.assetId = AssetId(groupObject.value(QLatin1String("assetId")).toString());
        group.displayName = groupObject.value(QLatin1String("displayName")).toString();
        group.stagingDirectoryName =
            groupObject.value(QLatin1String("stagingDirectoryName")).toString();

        for (const auto& memberValue : groupObject.value(QLatin1String("members")).toArray()) {
            const QJsonObject entry = memberValue.toObject();
            domain::PlannedMember member;
            member.memberId = MemberId(entry.value(QLatin1String("memberId")).toString());
            member.role = roleFromToken(entry.value(QLatin1String("role")).toString());
            member.sourcePath = entry.value(QLatin1String("sourcePath")).toString();
            member.fileName = entry.value(QLatin1String("fileName")).toString();
            member.expected.sizeBytes =
                entry.value(QLatin1String("sizeBytes")).toString().toLongLong();
            member.expected.modifiedMsecsUtc =
                entry.value(QLatin1String("modifiedMsecs")).toString().toLongLong();
            member.expected.native.device =
                entry.value(QLatin1String("nativeDevice")).toString().toULongLong();
            member.expected.native.fileId =
                entry.value(QLatin1String("nativeFileId")).toString().toULongLong();
            member.expected.native.known = entry.value(QLatin1String("nativeKnown")).toBool();
            group.members.append(member);
        }
        plan.groups.append(group);
    }
    return plan;
}

} // namespace

int SqliteRepository::targetSchemaVersion() {
    return 1;
}

SqliteRepository::SqliteRepository(QString databaseFile, QString connectionName)
    : databaseFile_(std::move(databaseFile)), connectionName_(std::move(connectionName)) {
    if (connectionName_.isEmpty()) {
        connectionName_ =
            QStringLiteral("cullfinch-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
}

SqliteRepository::~SqliteRepository() {
    close();
}

bool SqliteRepository::open(QString* error) {
    if (open_) {
        return true;
    }

    QDir().mkpath(QFileInfo(databaseFile_).absolutePath());

    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(databaseFile_);
    if (!database_.open()) {
        report(
            error,
            tr("The metadata database could not be opened: %1").arg(database_.lastError().text()));
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }

    QSqlQuery pragma(database_);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = FULL"));

    if (!migrate(error)) {
        database_.close();
        QSqlDatabase::removeDatabase(connectionName_);
        return false;
    }

    open_ = true;
    return true;
}

bool SqliteRepository::beginTransactionScope(QString* error) {
    if (transactionDepth_ == 0 && !database_.transaction()) {
        report(error,
               tr("A transaction could not be started: %1").arg(database_.lastError().text()));
        return false;
    }
    ++transactionDepth_;
    return true;
}

bool SqliteRepository::endTransactionScope(bool commit, QString* error) {
    --transactionDepth_;
    if (transactionDepth_ > 0) {
        // An enclosing runInTransaction call decides whether the connection's
        // transaction is committed or rolled back.
        return true;
    }
    if (!commit) {
        database_.rollback();
        return true;
    }
    if (!database_.commit()) {
        report(error,
               tr("Committing the transaction failed: %1").arg(database_.lastError().text()));
        return false;
    }
    return true;
}

bool SqliteRepository::runInTransaction(const std::function<bool()>& action, QString* error) {
    if (!beginTransactionScope(error)) {
        return false;
    }
    const bool ok = action();
    if (!endTransactionScope(ok, error)) {
        return false;
    }
    return ok;
}

void SqliteRepository::close() {
    if (database_.isOpen()) {
        database_.close();
    }
    if (!connectionName_.isEmpty() && QSqlDatabase::contains(connectionName_)) {
        database_ = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName_);
    }
    open_ = false;
}

bool SqliteRepository::migrate(QString* error) {
    // Migrations are transactional, ordered, and recorded. A backup belongs to
    // the caller's upgrade policy; this method never rewrites data in place
    // outside a transaction.
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations ("
                                   "version INTEGER PRIMARY KEY,"
                                   "applied_utc TEXT NOT NULL)"))) {
        return fail(error, query, tr("Creating the migration table"));
    }

    int current = 0;
    if (query.exec(QStringLiteral("SELECT COALESCE(MAX(version), 0) FROM schema_migrations")) &&
        query.next()) {
        current = query.value(0).toInt();
    }

    if (current > targetSchemaVersion()) {
        report(error, tr("The metadata database uses schema version %1; this build supports "
                         "version %2. Update cullfinch rather than downgrading the database.")
                          .arg(current)
                          .arg(targetSchemaVersion()));
        return false;
    }
    if (current == targetSchemaVersion()) {
        return true;
    }

    static const QStringList kVersion1 = {
        QStringLiteral("CREATE TABLE collections ("
                       "id TEXT PRIMARY KEY,"
                       "root_path TEXT NOT NULL UNIQUE,"
                       "recursive INTEGER NOT NULL DEFAULT 0,"
                       "association_config TEXT NOT NULL DEFAULT '',"
                       "revision INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE assets ("
                       "id TEXT PRIMARY KEY,"
                       "collection_id TEXT NOT NULL REFERENCES collections(id) ON DELETE CASCADE,"
                       "stem TEXT NOT NULL,"
                       "relative_directory TEXT NOT NULL,"
                       "display_name TEXT NOT NULL,"
                       "preview_member_id TEXT NOT NULL DEFAULT '',"
                       "pairing_state TEXT NOT NULL,"
                       "disposition TEXT NOT NULL,"
                       "membership_revision TEXT NOT NULL,"
                       "operations_blocked INTEGER NOT NULL DEFAULT 0,"
                       "diagnostics TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX idx_assets_collection_disposition "
                       "ON assets(collection_id, disposition)"),
        QStringLiteral("CREATE TABLE members ("
                       "id TEXT NOT NULL,"
                       "asset_id TEXT NOT NULL REFERENCES assets(id) ON DELETE CASCADE,"
                       "collection_id TEXT NOT NULL,"
                       "role TEXT NOT NULL,"
                       "absolute_path TEXT NOT NULL,"
                       "file_name TEXT NOT NULL,"
                       "extension TEXT NOT NULL,"
                       "size_bytes INTEGER NOT NULL,"
                       "modified_msecs INTEGER NOT NULL,"
                       "native_device TEXT NOT NULL DEFAULT '0',"
                       "native_file_id TEXT NOT NULL DEFAULT '0',"
                       "native_known INTEGER NOT NULL DEFAULT 0,"
                       "is_symlink INTEGER NOT NULL DEFAULT 0,"
                       "ordinal INTEGER NOT NULL DEFAULT 0,"
                       "PRIMARY KEY (id, asset_id))"),
        QStringLiteral("CREATE INDEX idx_members_collection_path "
                       "ON members(collection_id, absolute_path)"),
        QStringLiteral("CREATE TABLE sessions ("
                       "id TEXT PRIMARY KEY,"
                       "collection_id TEXT NOT NULL,"
                       "flow_id TEXT NOT NULL,"
                       "schema_version INTEGER NOT NULL,"
                       "state_revision TEXT NOT NULL,"
                       "snapshot TEXT NOT NULL,"
                       "draft_payload TEXT NOT NULL,"
                       "draft_rejected TEXT NOT NULL,"
                       "lifecycle TEXT NOT NULL,"
                       "created_utc TEXT NOT NULL,"
                       "updated_utc TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE INDEX idx_sessions_collection ON sessions(collection_id, lifecycle)"),
        QStringLiteral("CREATE TABLE operations ("
                       "id TEXT PRIMARY KEY,"
                       "collection_id TEXT NOT NULL,"
                       "plan TEXT NOT NULL,"
                       "state TEXT NOT NULL,"
                       "trash_path TEXT NOT NULL DEFAULT '',"
                       "error TEXT NOT NULL DEFAULT '',"
                       "created_utc TEXT NOT NULL,"
                       "updated_utc TEXT NOT NULL)"),
        QStringLiteral(
            "CREATE INDEX idx_operations_collection ON operations(collection_id, state)"),
        QStringLiteral("CREATE TABLE operation_members ("
                       "operation_id TEXT NOT NULL REFERENCES operations(id) ON DELETE CASCADE,"
                       "member_id TEXT NOT NULL,"
                       "asset_id TEXT NOT NULL,"
                       "source_path TEXT NOT NULL,"
                       "staging_path TEXT NOT NULL DEFAULT '',"
                       "last_step TEXT NOT NULL,"
                       "error TEXT NOT NULL DEFAULT '',"
                       "PRIMARY KEY (operation_id, member_id))")};

    if (!database_.transaction()) {
        report(
            error,
            tr("The database does not support transactions: %1").arg(database_.lastError().text()));
        return false;
    }

    for (const QString& statement : kVersion1) {
        QSqlQuery migration(database_);
        if (!migration.exec(statement)) {
            const QString message = migration.lastError().text();
            database_.rollback();
            report(error, tr("Applying schema version 1 failed: %1").arg(message));
            return false;
        }
    }

    QSqlQuery record(database_);
    record.prepare(QStringLiteral(
        "INSERT INTO schema_migrations (version, applied_utc) VALUES (:version, :applied)"));
    record.bindValue(QStringLiteral(":version"), 1);
    record.bindValue(QStringLiteral(":applied"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (!record.exec()) {
        const QString message = record.lastError().text();
        database_.rollback();
        report(error, tr("Recording the schema migration failed: %1").arg(message));
        return false;
    }

    if (!database_.commit()) {
        report(error,
               tr("Committing the schema migration failed: %1").arg(database_.lastError().text()));
        return false;
    }
    return true;
}

namespace {

/// The stored identity of a directory: its canonical path, or the absolute
/// spelling when there is nothing to resolve yet. It is the identity AppLock
/// uses, so the writer that opened the real path and a read-only instance
/// that opened a symlink to it agree on which row is theirs.
QString collectionIdentity(const QString& rootPath) {
    const QFileInfo info(rootPath);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

/// Looks the collection up under its identity and, for rows written before
/// the identity was canonical, under the absolute spelling as given.
///
/// Returns the row found, or nullopt; `*queryFailed` tells a failed query
/// from a row that is not there.
std::optional<CollectionId> lookupCollection(const QSqlDatabase& database, const QString& rootPath,
                                             QString* error, bool* queryFailed) {
    *queryFailed = false;
    QStringList spellings{collectionIdentity(rootPath)};
    if (const QString absolute = QFileInfo(rootPath).absoluteFilePath();
        absolute != spellings.constFirst()) {
        spellings.append(absolute);
    }
    for (const QString& spelling : std::as_const(spellings)) {
        QSqlQuery lookup(database);
        lookup.prepare(QStringLiteral("SELECT id FROM collections WHERE root_path = :root"));
        lookup.bindValue(QStringLiteral(":root"), text(spelling));
        if (!lookup.exec()) {
            fail(error, lookup, tr("Looking up the collection"));
            *queryFailed = true;
            return std::nullopt;
        }
        if (lookup.next()) {
            return CollectionId(lookup.value(0).toString());
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<CollectionId> SqliteRepository::findCollection(const QString& rootPath,
                                                             QString* error) const {
    bool queryFailed = false;
    std::optional<CollectionId> id = lookupCollection(database_, rootPath, error, &queryFailed);
    if (queryFailed) {
        return std::nullopt;
    }
    if (!id.has_value()) {
        report(error, tr("'%1' has not been opened by the window that has it open for writing "
                         "yet; there is nothing stored to browse.")
                          .arg(rootPath));
        return std::nullopt;
    }
    return id;
}

std::optional<CollectionId> SqliteRepository::ensureCollection(const QString& rootPath,
                                                               bool recursive, QString* error) {
    const QString canonical = collectionIdentity(rootPath);

    bool queryFailed = false;
    const std::optional<CollectionId> existing =
        lookupCollection(database_, rootPath, error, &queryFailed);
    if (queryFailed) {
        return std::nullopt;
    }
    if (existing.has_value()) {
        const CollectionId& id = *existing;
        QSqlQuery update(database_);
        update.prepare(
            QStringLiteral("UPDATE collections SET recursive = :recursive WHERE id = :id"));
        update.bindValue(QStringLiteral(":recursive"), recursive ? 1 : 0);
        update.bindValue(QStringLiteral(":id"), text(id.toString()));
        if (!update.exec()) {
            fail(error, update, tr("Updating the collection"));
            return std::nullopt;
        }
        return id;
    }

    // Deterministic identity, so reopening a directory reconciles onto the
    // same stored collection.
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(canonical.toUtf8());
    CollectionId id(QString::fromLatin1(hash.result().toHex()));

    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO collections (id, root_path, recursive, association_config, revision) "
        "VALUES (:id, :root, :recursive, '', 1)"));
    insert.bindValue(QStringLiteral(":id"), text(id.toString()));
    insert.bindValue(QStringLiteral(":root"), text(canonical));
    insert.bindValue(QStringLiteral(":recursive"), recursive ? 1 : 0);
    if (!insert.exec()) {
        fail(error, insert, tr("Creating the collection"));
        return std::nullopt;
    }
    return id;
}

quint64 SqliteRepository::collectionRevision(const CollectionId& id, QString* error) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT revision FROM collections WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!query.exec() || !query.next()) {
        report(error, tr("The collection revision could not be read."));
        return 0;
    }
    return query.value(0).toULongLong();
}

PhotoAssetList SqliteRepository::loadAssets(const CollectionId& id, QString* error) const {
    PhotoAssetList assets;

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT id, stem, relative_directory, display_name, preview_member_id, pairing_state,"
        " disposition, membership_revision, operations_blocked, diagnostics"
        " FROM assets WHERE collection_id = :collection ORDER BY relative_directory, stem"));
    query.bindValue(QStringLiteral(":collection"), text(id.toString()));
    if (!query.exec()) {
        fail(error, query, tr("Reading the collection"));
        return assets;
    }

    QHash<QString, int> indexById;
    while (query.next()) {
        PhotoAsset asset;
        asset.id = AssetId(query.value(0).toString());
        asset.collectionId = id;
        asset.stem = query.value(1).toString();
        asset.relativeDirectory = query.value(2).toString();
        asset.displayName = query.value(3).toString();
        asset.previewMemberId = MemberId(query.value(4).toString());
        asset.pairingState = pairingFromToken(query.value(5).toString());
        asset.disposition = query.value(6).toString() == QLatin1String("reject")
                                ? Disposition::Reject
                                : Disposition::Neutral;
        asset.membershipRevision = query.value(7).toString().toULongLong();
        asset.operationsBlocked = query.value(8).toInt() != 0;
        if (const QString diagnostics = query.value(9).toString(); !diagnostics.isEmpty()) {
            asset.diagnostics = diagnostics.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        }
        indexById.insert(asset.id.toString(), static_cast<int>(assets.size()));
        assets.append(asset);
    }

    QSqlQuery members(database_);
    members.prepare(QStringLiteral(
        "SELECT asset_id, id, role, absolute_path, file_name, extension, size_bytes,"
        " modified_msecs, native_device, native_file_id, native_known, is_symlink"
        " FROM members WHERE collection_id = :collection ORDER BY asset_id, ordinal"));
    members.bindValue(QStringLiteral(":collection"), text(id.toString()));
    if (!members.exec()) {
        fail(error, members, tr("Reading the file groups"));
        return assets;
    }

    while (members.next()) {
        const auto position = indexById.constFind(members.value(0).toString());
        if (position == indexById.constEnd()) {
            continue;
        }
        FileMember member;
        member.id = MemberId(members.value(1).toString());
        member.role = roleFromToken(members.value(2).toString());
        member.absolutePath = members.value(3).toString();
        member.fileName = members.value(4).toString();
        member.extensionLower = members.value(5).toString();
        member.fingerprint.sizeBytes = members.value(6).toLongLong();
        member.fingerprint.modifiedMsecsUtc = members.value(7).toLongLong();
        member.fingerprint.native.device = members.value(8).toString().toULongLong();
        member.fingerprint.native.fileId = members.value(9).toString().toULongLong();
        member.fingerprint.native.known = members.value(10).toInt() != 0;
        member.isSymlink = members.value(11).toInt() != 0;
        assets[*position].members.append(member);
    }

    return assets;
}

bool SqliteRepository::reconcileAssets(const CollectionId& id, const PhotoAssetList& scanned,
                                       PhotoAssetList* merged, quint64* newRevision,
                                       QString* error) {
    // A failed read must not be mistaken for "nothing stored yet". loadAssets
    // reports failure and returns an empty list, mergeWithStored would then find
    // no previous state for any photo, and the replacement transaction below
    // would delete every stored disposition -- losing the collection's deletion
    // marks to a transient read error. Refuse the reconciliation instead.
    QString readError;
    const PhotoAssetList stored = loadAssets(id, &readError);
    if (!readError.isEmpty()) {
        report(error, tr("The stored collection could not be read, so the scan results were not "
                         "applied: %1")
                          .arg(readError));
        return false;
    }

    const PhotoAssetList result = mergeWithStored(scanned, stored);

    if (!database_.transaction()) {
        report(error, tr("The scan results could not be stored: no transaction available."));
        return false;
    }

    const auto rollback = [this, error](const QSqlQuery& query, const QString& what) {
        const QString message = query.lastError().text();
        database_.rollback();
        report(error, tr("%1 failed: %2").arg(what, message));
        return false;
    };

    QSqlQuery clearMembers(database_);
    clearMembers.prepare(QStringLiteral("DELETE FROM members WHERE collection_id = :collection"));
    clearMembers.bindValue(QStringLiteral(":collection"), text(id.toString()));
    if (!clearMembers.exec()) {
        return rollback(clearMembers, tr("Clearing stored file groups"));
    }

    QSqlQuery clearAssets(database_);
    clearAssets.prepare(QStringLiteral("DELETE FROM assets WHERE collection_id = :collection"));
    clearAssets.bindValue(QStringLiteral(":collection"), text(id.toString()));
    if (!clearAssets.exec()) {
        return rollback(clearAssets, tr("Clearing stored photos"));
    }

    QSqlQuery insertAsset(database_);
    insertAsset.prepare(QStringLiteral(
        "INSERT INTO assets (id, collection_id, stem, relative_directory, display_name,"
        " preview_member_id, pairing_state, disposition, membership_revision,"
        " operations_blocked, diagnostics)"
        " VALUES (:id, :collection, :stem, :directory, :display, :preview, :pairing,"
        " :disposition, :revision, :blocked, :diagnostics)"));

    QSqlQuery insertMember(database_);
    insertMember.prepare(QStringLiteral(
        "INSERT INTO members (id, asset_id, collection_id, role, absolute_path, file_name,"
        " extension, size_bytes, modified_msecs, native_device, native_file_id, native_known,"
        " is_symlink, ordinal)"
        " VALUES (:id, :asset, :collection, :role, :path, :name, :extension, :size, :modified,"
        " :device, :fileId, :known, :symlink, :ordinal)"));

    for (const PhotoAsset& asset : result) {
        bindAsset(insertAsset, id, asset);
        if (!insertAsset.exec()) {
            return rollback(insertAsset, tr("Storing a photo"));
        }

        int ordinal = 0;
        for (const FileMember& member : asset.members) {
            bindMember(insertMember, id, asset, member, ordinal++);
            if (!insertMember.exec()) {
                return rollback(insertMember, tr("Storing a file group"));
            }
        }
    }

    QSqlQuery revision(database_);
    revision.prepare(
        QStringLiteral("UPDATE collections SET revision = revision + 1 WHERE id = :id"));
    revision.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!revision.exec()) {
        return rollback(revision, tr("Advancing the collection revision"));
    }

    if (!database_.commit()) {
        report(error, tr("Storing the scan results failed: %1").arg(database_.lastError().text()));
        return false;
    }

    if (merged != nullptr) {
        *merged = result;
    }
    if (newRevision != nullptr) {
        *newRevision = collectionRevision(id, error);
    }
    return true;
}

bool SqliteRepository::applyDispositionsLocked(const CollectionId& id, quint64 expectedRevision,
                                               const QList<AssetId>& reject,
                                               const QList<AssetId>& neutral,
                                               QString* error) const {
    if (const quint64 actual = collectionRevision(id, nullptr); actual != expectedRevision) {
        report(error, tr("The collection changed while you were working (revision %1, "
                         "expected %2). Refresh and try again.")
                          .arg(actual)
                          .arg(expectedRevision));
        return false;
    }

    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE assets SET disposition = :disposition WHERE id = :id AND "
                                  "collection_id = :collection"));
    if (!applyDisposition(update, id, reject, QStringLiteral("reject")) ||
        !applyDisposition(update, id, neutral, QStringLiteral("neutral"))) {
        report(error, tr("Storing deletion marks failed: %1").arg(update.lastError().text()));
        return false;
    }

    QSqlQuery revision(database_);
    revision.prepare(
        QStringLiteral("UPDATE collections SET revision = revision + 1 WHERE id = :id"));
    revision.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!revision.exec()) {
        report(error,
               tr("Advancing the collection revision failed: %1").arg(revision.lastError().text()));
        return false;
    }
    return true;
}

bool SqliteRepository::applyDispositions(const CollectionId& id, quint64 expectedRevision,
                                         const QList<AssetId>& reject,
                                         const QList<AssetId>& neutral, quint64* newRevision,
                                         QString* error) {
    // Routed through runInTransaction rather than a private BEGIN/COMMIT so a
    // caller can fold this write into a larger transaction (see
    // SessionController::finish, which must not let the collection's marks
    // become durable unless the session record that describes them does too).
    if (const bool committed = runInTransaction(
            [this, &id, expectedRevision, &reject, &neutral, error]() {
                return applyDispositionsLocked(id, expectedRevision, reject, neutral, error);
            },
            error);
        !committed) {
        return false;
    }
    if (newRevision != nullptr) {
        *newRevision = collectionRevision(id, error);
    }
    return true;
}

bool SqliteRepository::saveSession(const StoredSession& session, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO sessions (id, collection_id, flow_id, schema_version, state_revision,"
        " snapshot, draft_payload, draft_rejected, lifecycle, created_utc, updated_utc)"
        " VALUES (:id, :collection, :flow, :schema, :revision, :snapshot, :payload, :rejected,"
        " :lifecycle, :created, :updated)"
        " ON CONFLICT(id) DO UPDATE SET"
        " state_revision = excluded.state_revision,"
        " snapshot = excluded.snapshot,"
        " draft_payload = excluded.draft_payload,"
        " draft_rejected = excluded.draft_rejected,"
        " lifecycle = excluded.lifecycle,"
        " updated_utc = excluded.updated_utc"));

    const QDateTime now = QDateTime::currentDateTimeUtc();
    QJsonObject rejected;
    rejected.insert(QLatin1String("ids"), domain::toJsonArray(session.draftRejected));

    query.bindValue(QStringLiteral(":id"), text(session.id.toString()));
    query.bindValue(QStringLiteral(":collection"), text(session.collectionId.toString()));
    query.bindValue(QStringLiteral(":flow"), text(session.draft.flowId));
    query.bindValue(QStringLiteral(":schema"), session.draft.schemaVersion);
    query.bindValue(QStringLiteral(":revision"), QString::number(session.draft.revision));
    query.bindValue(QStringLiteral(":snapshot"), jsonToString(snapshotToJson(session.snapshot)));
    query.bindValue(QStringLiteral(":payload"), text(jsonToString(session.draft.payload)));
    query.bindValue(QStringLiteral(":rejected"), text(jsonToString(rejected)));
    query.bindValue(QStringLiteral(":lifecycle"),
                    text(application::sessionLifecycleToken(session.lifecycle)));
    query.bindValue(
        QStringLiteral(":created"),
        (session.createdUtc.isValid() ? session.createdUtc : now).toString(Qt::ISODate));
    query.bindValue(
        QStringLiteral(":updated"),
        (session.updatedUtc.isValid() ? session.updatedUtc : now).toString(Qt::ISODate));

    if (!query.exec()) {
        return fail(error, query, tr("Saving the comparison draft"));
    }
    return true;
}

namespace {

StoredSession sessionFromRow(const QSqlQuery& query) {
    StoredSession session;
    session.id = domain::SessionId(query.value(0).toString());
    session.collectionId = CollectionId(query.value(1).toString());
    session.draft.flowId = query.value(2).toString();
    session.draft.schemaVersion = query.value(3).toInt();
    session.draft.revision = query.value(4).toString().toULongLong();
    session.snapshot = snapshotFromJson(jsonFromString(query.value(5).toString()));
    session.draft.payload = jsonFromString(query.value(6).toString());
    session.draftRejected = domain::assetIdsFromJson(
        jsonFromString(query.value(7).toString()).value(QLatin1String("ids")).toArray());
    session.lifecycle = application::sessionLifecycleFromToken(query.value(8).toString());
    session.createdUtc = QDateTime::fromString(query.value(9).toString(), Qt::ISODate);
    session.updatedUtc = QDateTime::fromString(query.value(10).toString(), Qt::ISODate);
    return session;
}

constexpr auto kSessionColumns =
    "id, collection_id, flow_id, schema_version, state_revision, snapshot, draft_payload,"
    " draft_rejected, lifecycle, created_utc, updated_utc";

} // namespace

std::optional<StoredSession> SqliteRepository::loadSession(const domain::SessionId& id,
                                                           QString* error) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM sessions WHERE id = :id")
                      .arg(QLatin1String(kSessionColumns)));
    query.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!query.exec()) {
        fail(error, query, tr("Reading the comparison draft"));
        return std::nullopt;
    }
    if (!query.next()) {
        report(error, tr("No such comparison draft."));
        return std::nullopt;
    }
    return sessionFromRow(query);
}

QList<StoredSession> SqliteRepository::resumableSessions(const CollectionId& id,
                                                         QString* error) const {
    QList<StoredSession> sessions;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM sessions"
                                 " WHERE collection_id = :collection"
                                 " AND lifecycle IN ('active', 'paused')"
                                 " ORDER BY updated_utc DESC")
                      .arg(QLatin1String(kSessionColumns)));
    query.bindValue(QStringLiteral(":collection"), text(id.toString()));
    if (!query.exec()) {
        fail(error, query, tr("Reading saved comparison drafts"));
        return sessions;
    }
    while (query.next()) {
        sessions.append(sessionFromRow(query));
    }
    return sessions;
}

bool SqliteRepository::deleteSession(const domain::SessionId& id, QString* error) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!query.exec()) {
        return fail(error, query, tr("Discarding the comparison draft"));
    }
    return true;
}

bool SqliteRepository::saveOperation(const OperationRecord& record, QString* error) {
    if (!database_.transaction()) {
        report(error, tr("The operation journal could not be written: no transaction available."));
        return false;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO operations (id, collection_id, plan, state, trash_path, error, created_utc,"
        " updated_utc)"
        " VALUES (:id, :collection, :plan, :state, :trash, :error, :created, :updated)"
        " ON CONFLICT(id) DO UPDATE SET"
        " plan = excluded.plan,"
        " state = excluded.state,"
        " trash_path = excluded.trash_path,"
        " error = excluded.error,"
        " updated_utc = excluded.updated_utc"));

    const QDateTime now = QDateTime::currentDateTimeUtc();
    query.bindValue(QStringLiteral(":id"), text(record.plan.id.toString()));
    query.bindValue(QStringLiteral(":collection"), text(record.plan.collectionId.toString()));
    query.bindValue(QStringLiteral(":plan"), jsonToString(planToJson(record.plan)));
    query.bindValue(QStringLiteral(":state"), text(domain::operationStateToken(record.state)));
    query.bindValue(QStringLiteral(":trash"), text(record.trashPath));
    query.bindValue(QStringLiteral(":error"), text(record.error));
    query.bindValue(QStringLiteral(":created"),
                    (record.createdUtc.isValid() ? record.createdUtc : now).toString(Qt::ISODate));
    query.bindValue(QStringLiteral(":updated"),
                    (record.updatedUtc.isValid() ? record.updatedUtc : now).toString(Qt::ISODate));
    if (!query.exec()) {
        const QString message = query.lastError().text();
        database_.rollback();
        report(error, tr("Writing the operation record failed: %1").arg(message));
        return false;
    }

    QSqlQuery member(database_);
    member.prepare(QStringLiteral(
        "INSERT INTO operation_members (operation_id, member_id, asset_id, source_path,"
        " staging_path, last_step, error)"
        " VALUES (:operation, :member, :asset, :source, :staging, :step, :error)"
        " ON CONFLICT(operation_id, member_id) DO UPDATE SET"
        " staging_path = excluded.staging_path,"
        " last_step = excluded.last_step,"
        " error = excluded.error"));

    for (const OperationMemberRecord& entry : record.members) {
        member.bindValue(QStringLiteral(":operation"), text(record.plan.id.toString()));
        member.bindValue(QStringLiteral(":member"), text(entry.memberId.toString()));
        member.bindValue(QStringLiteral(":asset"), text(entry.assetId.toString()));
        member.bindValue(QStringLiteral(":source"), text(entry.sourcePath));
        member.bindValue(QStringLiteral(":staging"), text(entry.stagingPath));
        member.bindValue(QStringLiteral(":step"), text(entry.lastDurableStep));
        member.bindValue(QStringLiteral(":error"), text(entry.error));
        if (!member.exec()) {
            const QString message = member.lastError().text();
            database_.rollback();
            report(error, tr("Writing an operation journal entry failed: %1").arg(message));
            return false;
        }
    }

    if (!database_.commit()) {
        report(error,
               tr("Writing the operation journal failed: %1").arg(database_.lastError().text()));
        return false;
    }
    return true;
}

namespace {

OperationRecord operationFromRow(const QSqlQuery& query) {
    OperationRecord record;
    record.plan = planFromJson(jsonFromString(query.value(1).toString()));
    record.state = domain::operationStateFromToken(query.value(2).toString());
    record.trashPath = query.value(3).toString();
    record.error = query.value(4).toString();
    record.createdUtc = QDateTime::fromString(query.value(5).toString(), Qt::ISODate);
    record.updatedUtc = QDateTime::fromString(query.value(6).toString(), Qt::ISODate);
    return record;
}

} // namespace

std::optional<OperationRecord> SqliteRepository::loadOperation(const domain::OperationId& id,
                                                               QString* error) const {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT id, plan, state, trash_path, error, created_utc, updated_utc"
                       " FROM operations WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!query.exec()) {
        fail(error, query, tr("Reading the operation"));
        return std::nullopt;
    }
    if (!query.next()) {
        report(error, tr("No such operation."));
        return std::nullopt;
    }

    OperationRecord record = operationFromRow(query);

    QSqlQuery members(database_);
    members.prepare(
        QStringLiteral("SELECT member_id, asset_id, source_path, staging_path, last_step, error"
                       " FROM operation_members WHERE operation_id = :id"));
    members.bindValue(QStringLiteral(":id"), text(id.toString()));
    if (!members.exec()) {
        fail(error, members, tr("Reading the operation journal"));
        return record;
    }
    while (members.next()) {
        OperationMemberRecord entry;
        entry.memberId = MemberId(members.value(0).toString());
        entry.assetId = AssetId(members.value(1).toString());
        entry.sourcePath = members.value(2).toString();
        entry.stagingPath = members.value(3).toString();
        entry.lastDurableStep = members.value(4).toString();
        entry.error = members.value(5).toString();
        record.members.append(entry);
    }
    return record;
}

QList<OperationRecord> SqliteRepository::unfinishedOperations(const CollectionId& id,
                                                              QString* error) const {
    QList<OperationRecord> records;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT id FROM operations WHERE collection_id = :collection AND state <> 'completed'"
        " ORDER BY created_utc"));
    query.bindValue(QStringLiteral(":collection"), text(id.toString()));
    if (!query.exec()) {
        fail(error, query, tr("Reading unfinished operations"));
        return records;
    }
    QList<domain::OperationId> ids;
    while (query.next()) {
        ids.append(domain::OperationId(query.value(0).toString()));
    }
    for (const domain::OperationId& operationId : ids) {
        const std::optional<OperationRecord> record = loadOperation(operationId, error);
        if (record.has_value()) {
            records.append(*record);
        }
    }
    return records;
}

} // namespace cullfinch::infrastructure
