// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/StagingExecutor.h>

#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStringList>

#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace cullfinch::infrastructure {
namespace {

using application::JournalWriter;
using application::OperationMemberRecord;
using application::OperationRecord;
namespace step = application::operationStep;
using domain::OperationState;
using domain::PlannedGroup;
using domain::PlannedMember;

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

std::filesystem::path toPath(const QString& value) {
    return std::filesystem::path(value.toStdString());
}

domain::FileFingerprint fingerprintOf(const QString& path) {
    domain::FileFingerprint fingerprint;
    const QFileInfo info(path);
    if (!info.exists()) {
        return fingerprint;
    }
    fingerprint.sizeBytes = info.size();
    fingerprint.modifiedMsecsUtc = info.lastModified().toMSecsSinceEpoch();
    return fingerprint;
}

/// The filesystem a path lives on, or nullopt when the platform cannot say.
///
/// Rename-only staging is meaningless across filesystems, so the check has to
/// ask the filesystem itself rather than compare path prefixes: a mount point
/// inside the collection looks like an ordinary subdirectory by name.
std::optional<quint64> deviceOf(const QString& path) {
    struct stat status{};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0) {
        return std::nullopt;
    }
    return static_cast<quint64>(status.st_dev);
}

/// Rename-only semantics on the same filesystem. A cross-device rename is
/// reported as such and never silently becomes copy-and-delete.
bool renameOnly(const QString& from, const QString& to, QString* error) {
    std::error_code code;
    if (std::filesystem::exists(toPath(to), code)) {
        // The operation's own new directories avoid ordinary collisions, so
        // unexpected contents are a conflict, not something to overwrite.
        *error = tr("'%1' already exists; nothing was overwritten.").arg(to);
        return false;
    }
    std::filesystem::rename(toPath(from), toPath(to), code);
    if (code) {
        *error = tr("Moving '%1' to '%2' failed: %3")
                     .arg(from, to, QString::fromStdString(code.message()));
        return false;
    }
    return true;
}

QString writeManifest(const PlannedGroup& group, const QString& directory,
                      const QList<OperationMemberRecord>& members, QString* error) {
    QJsonArray entries;
    for (const PlannedMember& member : group.members) {
        QString staged;
        for (const OperationMemberRecord& record : members) {
            if (record.memberId == member.memberId && record.lastDurableStep == step::staged) {
                staged = record.stagingPath;
                break;
            }
        }
        QJsonObject entry;
        entry.insert(QLatin1String("memberId"), member.memberId.toString());
        entry.insert(QLatin1String("fileName"), member.fileName);
        entry.insert(QLatin1String("originalPath"), member.sourcePath);
        entry.insert(QLatin1String("stagingPath"), staged);
        entry.insert(QLatin1String("sizeBytes"), QString::number(member.expected.sizeBytes));
        entries.append(entry);
    }

    QJsonObject manifest;
    manifest.insert(QLatin1String("version"), 1);
    manifest.insert(QLatin1String("assetId"), group.assetId.toString());
    manifest.insert(QLatin1String("displayName"), group.displayName);
    manifest.insert(QLatin1String("members"), entries);

    QString path = QDir(directory).absoluteFilePath(StagingExecutor::manifestFileName());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = tr("The recovery manifest could not be created: %1").arg(file.errorString());
        return QString();
    }
    file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    // Atomic replacement plus the durability synchronisation the recovery
    // policy needs: without the commit the manifest may not survive a crash.
    if (!file.commit()) {
        *error = tr("The recovery manifest could not be written: %1").arg(file.errorString());
        return QString();
    }
    return path;
}

OperationRecord withState(OperationRecord record, OperationState state, const QString& error) {
    record.state = state;
    record.error = error;
    return record;
}

int indexOfMember(const OperationRecord& record, const domain::MemberId& id) {
    for (int i = 0; i < record.members.size(); ++i) {
        if (record.members.at(i).memberId == id) {
            return i;
        }
    }
    return -1;
}

/// The journal entry for one member, or nullptr when the record does not carry
/// it. Every step below updates the journal through this, so "the record does
/// not know this member" stays one check rather than an index test per site.
OperationMemberRecord* entryFor(OperationRecord& record, const domain::MemberId& id) {
    const int index = indexOfMember(record, id);
    return index >= 0 ? &record.members[index] : nullptr;
}

/// Why the files of one group no longer match what was reviewed, or an empty
/// string when they still do.
QString memberBlocker(const PlannedGroup& group, const domain::PhotoAsset& asset,
                      const std::optional<quint64>& stagingDevice, const QString& stagingRoot) {
    // Membership must still match what was reviewed. A missing previously known
    // RAW blocks the whole group.
    QSet<QString> currentPaths;
    for (const domain::FileMember& member : asset.members) {
        currentPaths.insert(member.absolutePath);
    }

    for (const PlannedMember& member : group.members) {
        if (!currentPaths.contains(member.sourcePath)) {
            return tr("'%1' is no longer part of this photo.").arg(member.fileName);
        }
        const domain::FileFingerprint actual = fingerprintOf(member.sourcePath);
        if (!actual.isKnown()) {
            return tr("'%1' no longer exists.").arg(member.fileName);
        }
        if (actual.sizeBytes != member.expected.sizeBytes ||
            actual.modifiedMsecsUtc != member.expected.modifiedMsecsUtc) {
            return tr("'%1' changed on disk since this operation was reviewed.")
                .arg(member.fileName);
        }

        // Rename-only staging cannot cross a filesystem boundary. The spec asks
        // for this to block the policy rather than fail at member N with half
        // the group already moved.
        if (!stagingDevice.has_value()) {
            continue;
        }
        const std::optional<quint64> sourceDevice = deviceOf(member.sourcePath);
        if (!sourceDevice.has_value() || *sourceDevice != *stagingDevice) {
            return tr("'%1' is on a different filesystem from the staging directory '%2'. This "
                      "deletion policy moves files by rename only, so it cannot move this photo.")
                .arg(member.fileName, stagingRoot);
        }
    }

    if (group.members.size() != asset.members.size()) {
        return tr("This photo now has a different number of files than was reviewed.");
    }
    return {};
}

/// Re-enumerate the directory: a companion that appeared after the last scan is
/// not in the plan, and moving the rest would orphan it.
QString appearedSiblingBlocker(const PlannedGroup& group, const domain::AssociationConfig& config) {
    if (group.members.isEmpty()) {
        return {};
    }

    QSet<QString> planned;
    for (const PlannedMember& member : group.members) {
        planned.insert(QFileInfo(member.sourcePath).absoluteFilePath());
    }
    const QFileInfo firstMember(group.members.first().sourcePath);
    const QString stem = firstMember.completeBaseName();
    const QFileInfoList siblings =
        firstMember.dir().entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
    for (const QFileInfo& sibling : siblings) {
        // The same policy as the scan: a switched-off sidecar is not part of the
        // photo and never was, so it cannot be left behind; an unrecognised
        // same-stem file, on the other hand, is exactly what blocks a group.
        if (config.isDisabledSidecar(sibling.suffix())) {
            continue;
        }
        if (sibling.completeBaseName() == stem && !planned.contains(sibling.absoluteFilePath())) {
            return tr("'%1' appeared beside this photo after it was reviewed.")
                .arg(sibling.fileName());
        }
    }
    return {};
}

/// Record where every member is going, and that it has not gone yet.
void recordStagingIntent(OperationRecord& record, const PlannedGroup& group,
                         const QString& directory) {
    for (const PlannedMember& member : group.members) {
        if (OperationMemberRecord* entry = entryFor(record, member.memberId); entry != nullptr) {
            entry->stagingPath = QDir(directory).absoluteFilePath(member.fileName);
            entry->lastDurableStep = step::planned;
        }
    }
}

/// Put back everything that already reached staging, and report what would not
/// go back. Never overwrites: a staging copy that cannot be restored is retained
/// and shown as a recovery task.
QStringList putBackMoved(OperationRecord& record, const QList<PlannedMember>& moved,
                         const QString& directory) {
    QStringList problems;
    for (const PlannedMember& member : moved) {
        const QString staged = QDir(directory).absoluteFilePath(member.fileName);
        if (QString restoreError; !renameOnly(staged, member.sourcePath, &restoreError)) {
            problems.append(restoreError);
            continue;
        }
        if (OperationMemberRecord* entry = entryFor(record, member.memberId); entry != nullptr) {
            entry->stagingPath.clear();
            entry->lastDurableStep = step::restored;
        }
    }
    return problems;
}

/// Stop a group after a failed rename.
///
/// Puts back everything that already moved, records the error against the member
/// that failed, and returns the state the group ends in: Failed when the source
/// tree is intact again, NeedsRecovery when something could not go back.
OperationRecord abortStaging(OperationRecord record, const PlannedMember& failed,
                             const QString& moveError, const QList<PlannedMember>& movedMembers,
                             const QString& directory) {
    const QStringList restoreProblems = putBackMoved(record, movedMembers, directory);
    if (OperationMemberRecord* entry = entryFor(record, failed.memberId); entry != nullptr) {
        entry->error = moveError;
    }
    if (restoreProblems.isEmpty()) {
        return withState(record, OperationState::Failed, moveError);
    }
    return withState(record, OperationState::NeedsRecovery,
                     tr("%1 Some files could not be put back: %2")
                         .arg(moveError, restoreProblems.join(QLatin1String(" "))));
}

/// The first member that failed to arrive in staging with its reviewed identity.
///
/// Returns the file's name, so the answer is an optional rather than an empty
/// string: a name is data and could in principle be empty, where "no blocker"
/// cannot be.
std::optional<QString> absentFromStaging(const PlannedGroup& group, const QString& directory) {
    for (const PlannedMember& member : group.members) {
        const QString staged = QDir(directory).absoluteFilePath(member.fileName);
        const domain::FileFingerprint actual = fingerprintOf(staged);
        if (!actual.isKnown() || actual.sizeBytes != member.expected.sizeBytes ||
            actual.modifiedMsecsUtc != member.expected.modifiedMsecsUtc) {
            return member.fileName;
        }
    }
    return std::nullopt;
}

/// True when the journal already recorded every member of the group in Trash.
bool journalSaysTrashed(const OperationRecord& record, const PlannedGroup& group) {
    if (group.members.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(group.members, [&record](const PlannedMember& member) {
        const int index = indexOfMember(record, member.memberId);
        return index >= 0 && record.members.at(index).lastDurableStep == step::trashed;
    });
}

/// What one group's restoration attempt found.
struct GroupRestore {
    QStringList missing;  ///< Members in neither their original nor staging location.
    QStringList problems; ///< Everything that needs a person.
    bool anyFound = false;
    int stillStaged = 0;
    int restored = 0;
};

/// Where the member's staged copy should be.
///
/// The journal first, then the manifest beside the files, then the planned name:
/// a crash between a rename and its confirmation leaves the journal one step
/// behind, and the manifest, or failing that the plan, still says where the file
/// was going.
QString stagedPathFor(const OperationMemberRecord& entry, const std::optional<Manifest>& manifest,
                      const PlannedMember& member, const QString& directory) {
    if (!entry.stagingPath.isEmpty()) {
        return entry.stagingPath;
    }
    if (manifest.has_value()) {
        const ManifestEntry* known = manifest->entry(member.memberId);
        if (known != nullptr && !known->stagingPath.isEmpty()) {
            return known->stagingPath;
        }
    }
    return QDir(directory).absoluteFilePath(member.fileName);
}

void restoreMember(const PlannedMember& member, const QString& staged, OperationMemberRecord& entry,
                   GroupRestore& found) {
    const bool sourceExists = QFileInfo::exists(entry.sourcePath);
    const bool stagedExists = QFileInfo::exists(staged);
    if (sourceExists && !stagedExists) {
        // Never moved, or already put back.
        found.anyFound = true;
        entry.stagingPath.clear();
        entry.lastDurableStep = step::planned;
        return;
    }
    if (!stagedExists) {
        found.missing.append(member.fileName);
        return;
    }
    found.anyFound = true;
    if (sourceExists) {
        found.problems.append(tr("'%1' exists both at its original path and in staging; "
                                 "nothing was overwritten.")
                                  .arg(member.fileName));
        ++found.stillStaged;
        return;
    }

    // A file in staging is only put back when it is the file that was reviewed.
    // Existence alone proves nothing after a crash.
    if (const domain::FileFingerprint actual = fingerprintOf(staged);
        !actual.isKnown() || actual.sizeBytes != member.expected.sizeBytes ||
        actual.modifiedMsecsUtc != member.expected.modifiedMsecsUtc) {
        found.problems.append(tr("'%1' is in staging but is not the file that was reviewed "
                                 "(its size or modification time differs); it was left "
                                 "where it is.")
                                  .arg(member.fileName));
        entry.stagingPath = staged;
        ++found.stillStaged;
        return;
    }

    if (QString moveError; !renameOnly(staged, entry.sourcePath, &moveError)) {
        found.problems.append(moveError);
        entry.stagingPath = staged;
        ++found.stillStaged;
        return;
    }
    entry.stagingPath.clear();
    entry.lastDurableStep = step::restored;
    ++found.restored;
}

GroupRestore restoreGroup(OperationRecord& record, const PlannedGroup& group,
                          const QString& directory) {
    QString manifestError;
    const std::optional<Manifest> manifest =
        StagingExecutor::readManifest(directory, &manifestError);

    GroupRestore found;
    for (const PlannedMember& member : group.members) {
        OperationMemberRecord* entry = entryFor(record, member.memberId);
        if (entry == nullptr) {
            continue;
        }
        restoreMember(member, stagedPathFor(*entry, manifest, member, directory), *entry, found);
    }
    return found;
}

void markGroupTrashed(OperationRecord& record, const PlannedGroup& group) {
    for (const PlannedMember& member : group.members) {
        if (OperationMemberRecord* entry = entryFor(record, member.memberId); entry != nullptr) {
            entry->lastDurableStep = step::trashed;
        }
    }
}

/// Where this group's files were staged.
QString stagingDirectoryOf(const OperationRecord& record, const PlannedGroup& group) {
    return QDir(record.plan.stagingRoot).absoluteFilePath(group.stagingDirectoryName);
}

/// True when every member of the group is at the step only a person can settle.
bool journalSaysUncertain(const OperationRecord& record, const PlannedGroup& group) {
    if (group.members.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(group.members, [&record](const PlannedMember& member) {
        const int index = indexOfMember(record, member.memberId);
        return index >= 0 && record.members.at(index).lastDurableStep == step::uncertain;
    });
}

/// Decide what a group that vanished whole means, and report whether it is
/// confirmed to have reached Trash.
///
/// A group directory that disappeared together is what a Trash of it looks like.
/// That is confirmed only when the reported Trash location still holds this
/// group's manifest; otherwise the outcome is recorded as uncertain and left to
/// a person. Nothing is ever deleted again on a guess.
bool vanishedGroupReachedTrash(OperationRecord& record, const PlannedGroup& group,
                               QStringList& problems) {
    std::optional<Manifest> inTrash;
    if (!record.trashPath.isEmpty()) {
        QString ignored;
        inTrash = StagingExecutor::readManifest(record.trashPath, &ignored);
    }
    if (inTrash.has_value() && inTrash->assetId == group.assetId) {
        markGroupTrashed(record, group);
        return true;
    }

    problems.append(tr("'%1' (%2 file(s)) is in neither its original location nor staging. "
                       "It was probably moved to Trash before that could be recorded; check "
                       "the Trash before doing anything else. Nothing will be deleted again.")
                        .arg(group.displayName)
                        .arg(group.members.size()));
    for (const PlannedMember& member : group.members) {
        if (OperationMemberRecord* entry = entryFor(record, member.memberId); entry != nullptr) {
            // A step of its own: this is not "staged", and the difference is
            // what lets the recovery screen offer the one thing that settles
            // it -- a person saying they have seen the group in Trash.
            entry->lastDurableStep = step::uncertain;
            entry->error = tr("Not found in staging or at the original path.");
        }
    }
    return false;
}

} // namespace

const ManifestEntry* Manifest::entry(const domain::MemberId& id) const {
    for (const ManifestEntry& candidate : members) {
        if (candidate.memberId == id) {
            return &candidate;
        }
    }
    return nullptr;
}

QString StagingExecutor::manifestFileName() {
    return QStringLiteral("cullfinch-manifest.json");
}

std::optional<Manifest> StagingExecutor::readManifest(const QString& directory, QString* error) {
    const QString path = QDir(directory).absoluteFilePath(manifestFileName());
    QFile file(path);
    if (!file.exists()) {
        if (error != nullptr) {
            *error = tr("There is no recovery manifest in '%1'.").arg(directory);
        }
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = tr("The recovery manifest '%1' could not be read: %2")
                         .arg(path, file.errorString());
        }
        return std::nullopt;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = tr("The recovery manifest '%1' is not readable: %2")
                         .arg(path, parseError.errorString());
        }
        return std::nullopt;
    }

    const QJsonObject object = document.object();
    if (object.value(QLatin1String("version")).toInt() != 1) {
        if (error != nullptr) {
            *error = tr("The recovery manifest '%1' was written by an unknown version.").arg(path);
        }
        return std::nullopt;
    }

    Manifest manifest;
    manifest.assetId = domain::AssetId(object.value(QLatin1String("assetId")).toString());
    manifest.displayName = object.value(QLatin1String("displayName")).toString();
    const QJsonArray entries = object.value(QLatin1String("members")).toArray();
    for (const auto& value : entries) {
        const QJsonObject item = value.toObject();
        ManifestEntry entry;
        entry.memberId = domain::MemberId(item.value(QLatin1String("memberId")).toString());
        entry.fileName = item.value(QLatin1String("fileName")).toString();
        entry.originalPath = item.value(QLatin1String("originalPath")).toString();
        entry.stagingPath = item.value(QLatin1String("stagingPath")).toString();
        entry.sizeBytes = item.value(QLatin1String("sizeBytes")).toString().toLongLong();
        manifest.members.append(entry);
    }
    return manifest;
}

StagingExecutor::StagingExecutor(application::ITrashAdapter& trash) : trash_(trash) {}

void StagingExecutor::setAssociationConfig(const domain::AssociationConfig& config) {
    config_ = config;
}

domain::PlanningResult StagingExecutor::preflight(const domain::OperationPlan& plan,
                                                  const domain::PhotoAssetList& current,
                                                  QString* error) const {
    domain::PlanningResult result;
    result.plan = plan;

    QHash<domain::AssetId, const domain::PhotoAsset*> byId;
    for (const domain::PhotoAsset& asset : current) {
        byId.insert(asset.id, &asset);
    }

    // The staging root must be writable and on the source filesystem.
    std::optional<quint64> stagingDevice;
    if (!plan.stagingRoot.isEmpty()) {
        QDir().mkpath(plan.stagingRoot);
        if (!QFileInfo(plan.stagingRoot).isWritable()) {
            if (error != nullptr) {
                *error = tr("The staging directory '%1' is not writable. This deletion policy "
                            "needs a writable location on the same filesystem as the photos.")
                             .arg(plan.stagingRoot);
            }
            return result;
        }
        stagingDevice = deviceOf(plan.stagingRoot);
    }

    for (const PlannedGroup& group : plan.groups) {
        const domain::PhotoAsset* asset = byId.value(group.assetId, nullptr);
        if (asset == nullptr) {
            result.blocked.append(
                domain::PlanningIssue{group.assetId, group.displayName,
                                      tr("This photo is no longer in the collection.")});
            continue;
        }
        if (!asset->isOperable()) {
            result.blocked.append(
                domain::PlanningIssue{group.assetId, group.displayName,
                                      tr("The file group changed and must be resolved again.")});
            continue;
        }
        if (asset->disposition != domain::Disposition::Reject) {
            result.blocked.append(
                domain::PlanningIssue{group.assetId, group.displayName,
                                      tr("This photo is no longer marked for deletion.")});
            continue;
        }

        QString blocker = memberBlocker(group, *asset, stagingDevice, plan.stagingRoot);
        if (blocker.isEmpty()) {
            blocker = appearedSiblingBlocker(group, config_);
        }
        if (!blocker.isEmpty()) {
            result.blocked.append(domain::PlanningIssue{group.assetId, group.displayName, blocker});
        }
    }

    return result;
}

OperationRecord StagingExecutor::executeGroup(const OperationRecord& input,
                                              const PlannedGroup& group,
                                              const JournalWriter& journal) {
    OperationRecord record = input;
    record.state = OperationState::Staging;

    const auto writeJournal = [&journal, &record](QString* journalError) {
        if (!journal) {
            return true;
        }
        QString message;
        if (journal(record, &message)) {
            return true;
        }
        *journalError = tr("The operation journal could not be written: %1").arg(message);
        return false;
    };

    const QString directory =
        QDir(record.plan.stagingRoot).absoluteFilePath(group.stagingDirectoryName);
    if (!QDir().mkpath(directory)) {
        return withState(record, OperationState::Failed,
                         tr("The staging directory '%1' could not be created.").arg(directory));
    }

    // The manifest is written before anything moves, so an interrupted run is
    // always explainable from the staging directory alone.
    QString manifestError;
    if (writeManifest(group, directory, record.members, &manifestError).isEmpty()) {
        return withState(record, OperationState::Failed, manifestError);
    }

    // Record the intent for every member -- where it is going, and that it
    // has not gone yet -- before the first rename. A crash between a rename
    // and its confirmation then leaves a journal that names the destination
    // to look in, rather than one that swears the file never moved.
    recordStagingIntent(record, group, directory);
    QString journalError;
    if (!writeJournal(&journalError)) {
        return withState(record, OperationState::Failed, journalError); // Nothing has moved.
    }

    QList<PlannedMember> movedMembers;
    for (const PlannedMember& member : group.members) {
        const QString destination = QDir(directory).absoluteFilePath(member.fileName);
        if (QString moveError; !renameOnly(member.sourcePath, destination, &moveError)) {
            // Stop the group and attempt journalled restoration of what moved.
            return abortStaging(record, member, moveError, movedMembers, directory);
        }

        if (OperationMemberRecord* entry = entryFor(record, member.memberId); entry != nullptr) {
            entry->stagingPath = destination;
            entry->lastDurableStep = step::staged;
        }
        movedMembers.append(member);

        // Confirm the move before the next one starts, so the journal never
        // trails the filesystem by more than one rename.
        if (!writeJournal(&journalError)) {
            return withState(record, OperationState::NeedsRecovery,
                             tr("%1 '%2' had already been moved to staging; the files are kept "
                                "there for recovery and nothing was sent to Trash.")
                                 .arg(journalError, member.fileName));
        }
    }

    // Verify that every expected member reached staging with its recorded
    // identity before the group is called staged.
    if (const std::optional<QString> absent = absentFromStaging(group, directory);
        absent.has_value()) {
        return withState(record, OperationState::NeedsRecovery,
                         tr("'%1' did not arrive in staging as expected. The files are kept "
                            "for recovery and nothing was sent to Trash.")
                             .arg(*absent));
    }

    // Refresh the manifest so it records the actual staging paths.
    if (writeManifest(group, directory, record.members, &manifestError).isEmpty()) {
        return withState(record, OperationState::NeedsRecovery, manifestError);
    }

    record.state = OperationState::Staged;
    if (!writeJournal(&journalError)) {
        return withState(record, OperationState::NeedsRecovery, journalError);
    }

    // Trash is about to be asked. Recording that first is what lets recovery
    // tell "the group vanished because Trash took it" from "the group was
    // lost": after this point a missing directory is a probable Trash
    // outcome, never something to repeat. The path is per group: one left
    // over from the previous group would send recovery to the wrong manifest.
    record.trashPath.clear();
    record.state = OperationState::Trashing;
    if (!writeJournal(&journalError)) {
        return withState(record, OperationState::NeedsRecovery, journalError);
    }

    QString trashPath;
    if (QString trashError; !trash_.moveToTrash(directory, &trashPath, &trashError)) {
        // The complete group is retained. Retry Trash or Restore are the only
        // options; there is never an automatic fall back to permanent deletion.
        return withState(record, OperationState::NeedsRecovery,
                         tr("The photo was staged but could not be moved to Trash: %1 The "
                            "complete group is kept in '%2'; retry Trash or restore it.")
                             .arg(trashError, directory));
    }

    // The platform need not report a Trash path, so recovery relies on the
    // manifest inside the group directory rather than on this value.
    record.trashPath = std::move(trashPath);
    markGroupTrashed(record, group);

    // The outcome is journalled here, not left to the caller: a crash between
    // this return and the controller's own write would otherwise leave a
    // record that says "Trashing" with no Trash path, and recovery could only
    // call the vanished group uncertain. If even this write fails, the files
    // are in Trash but the record does not know it, which is exactly what
    // NeedsRecovery is for.
    if (!writeJournal(&journalError)) {
        return withState(record, OperationState::NeedsRecovery,
                         tr("%1 The photo reached Trash, but that could not be recorded; "
                            "recovery will confirm it from the manifest.")
                             .arg(journalError));
    }

    // The operation as a whole is only completed once every group is done; the
    // controller makes that call.
    return withState(record, OperationState::Trashing, QString());
}

OperationRecord StagingExecutor::recover(const OperationRecord& input) {
    OperationRecord record = input;
    record.state = OperationState::Restoring;

    QStringList problems;
    int stillStaged = 0;
    int trashedGroups = 0;
    int restoredGroups = 0;

    for (const PlannedGroup& group : record.plan.groups) {
        // Members the journal already saw reach Trash need nothing.
        if (journalSaysTrashed(record, group)) {
            ++trashedGroups;
            continue;
        }

        const QString directory = stagingDirectoryOf(record, group);
        const GroupRestore found = restoreGroup(record, group, directory);
        problems.append(found.problems);
        stillStaged += found.stillStaged;

        if (found.missing.isEmpty()) {
            if (found.restored > 0 || found.anyFound) {
                ++restoredGroups;
            }
            continue;
        }

        if (!found.anyFound && !QFileInfo::exists(directory)) {
            if (vanishedGroupReachedTrash(record, group, problems)) {
                ++trashedGroups;
            }
            continue;
        }

        // Part of the group is accounted for and part is not. Do not assume the
        // last recorded step completed: this needs a person.
        for (const QString& fileName : found.missing) {
            problems.append(
                tr("'%1' is in neither its original nor its staging location.").arg(fileName));
        }
    }

    if (!problems.isEmpty() || stillStaged > 0) {
        return withState(record, OperationState::NeedsRecovery, problems.join(QLatin1String(" ")));
    }
    if (trashedGroups == record.plan.groups.size()) {
        // Every group is known to have reached Trash; only the commit was
        // missing.
        return withState(record, OperationState::Completed, QString());
    }
    if (trashedGroups > 0) {
        // Some groups reached Trash before the interruption and the rest are
        // back at their original paths. The plan cannot simply be retried:
        // it still names the trashed groups. Nothing is left in staging.
        return withState(record, OperationState::Failed,
                         tr("%1 photo(s) were moved to Trash before the operation was "
                            "interrupted; %2 were put back and are still marked for deletion. "
                            "Review file operations again to move them.")
                             .arg(trashedGroups)
                             .arg(restoredGroups));
    }
    // Everything is back at its original path, so the reviewed plan is intact
    // and may be retried. Reporting this as "completed" would falsely imply the
    // photos reached Trash.
    return withState(record, OperationState::Planned, QString());
}

OperationRecord StagingExecutor::retryTrash(const OperationRecord& input) {
    OperationRecord record = input;

    // Every retry revalidates its preconditions, and this one validates the
    // whole plan before it asks Trash for anything. A group left at its
    // original paths means the plan no longer describes the work on disk;
    // trashing the rest would turn a repairable interruption into a half-done
    // operation nobody planned.
    QList<qsizetype> retryable;
    QStringList problems;
    for (qsizetype index = 0; index < record.plan.groups.size(); ++index) {
        const PlannedGroup& group = record.plan.groups.at(index);
        if (journalSaysTrashed(record, group)) {
            continue;
        }

        const QString directory = stagingDirectoryOf(record, group);
        if (!QFileInfo::exists(directory)) {
            problems.append(tr("'%1' is not in staging, so Trash cannot be asked for it again. "
                               "Restore this operation and review it again.")
                                .arg(group.displayName));
            continue;
        }
        // Existence proves nothing after a crash: only the group that was
        // reviewed, complete and unchanged, is handed to Trash.
        if (const std::optional<QString> absent = absentFromStaging(group, directory);
            absent.has_value()) {
            problems.append(tr("'%1' is not complete in staging ('%2' is missing or is no longer "
                               "the file that was reviewed), so it was not moved to Trash.")
                                .arg(group.displayName, *absent));
            continue;
        }
        retryable.append(index);
    }

    if (!problems.isEmpty()) {
        return withState(record, OperationState::NeedsRecovery, problems.join(QLatin1String(" ")));
    }

    record.state = OperationState::Trashing;
    for (const qsizetype index : retryable) {
        const PlannedGroup& group = record.plan.groups.at(index);
        const QString directory = stagingDirectoryOf(record, group);

        // Cleared per group, exactly as execution does: a path left over from
        // the previous group would send recovery to the wrong manifest.
        record.trashPath.clear();
        QString trashPath;
        if (QString trashError; !trash_.moveToTrash(directory, &trashPath, &trashError)) {
            // The complete group is retained. Retry Trash or Restore remain the
            // only options; there is never a fall back to permanent deletion.
            return withState(record, OperationState::NeedsRecovery,
                             tr("The photo was staged but could not be moved to Trash: %1 The "
                                "complete group is kept in '%2'; retry Trash or restore it.")
                                 .arg(trashError, directory));
        }
        record.trashPath = std::move(trashPath);
        markGroupTrashed(record, group);
    }
    return withState(record, OperationState::Completed, QString());
}

OperationRecord StagingExecutor::confirmTrashed(const OperationRecord& input) {
    OperationRecord record = input;

    int confirmed = 0;
    for (const PlannedGroup& group : record.plan.groups) {
        if (!journalSaysUncertain(record, group)) {
            continue;
        }
        markGroupTrashed(record, group);
        for (const PlannedMember& member : group.members) {
            if (OperationMemberRecord* entry = entryFor(record, member.memberId);
                entry != nullptr) {
                entry->error.clear();
            }
        }
        ++confirmed;
    }

    if (confirmed == 0) {
        return withState(record, OperationState::NeedsRecovery,
                         tr("No photo in this operation is waiting for a Trash outcome to be "
                            "confirmed."));
    }

    // Nothing was moved here: a person answered the one question the filesystem
    // cannot. Reconciling through recovery is what turns that answer into a
    // state, and it re-reads the disk rather than trusting the claim for the
    // groups nobody was asked about.
    return recover(record);
}

} // namespace cullfinch::infrastructure
