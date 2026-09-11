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

#include <filesystem>
#include <system_error>

namespace cullfinch::infrastructure {
namespace {

using application::JournalWriter;
using application::OperationMemberRecord;
using application::OperationRecord;
using domain::OperationState;
using domain::PlannedGroup;
using domain::PlannedMember;

QString tr(const char* text) {
    return QCoreApplication::translate("cullfinch", text);
}

constexpr auto kStepPlanned = "planned";
constexpr auto kStepStaged = "staged";
constexpr auto kStepRestored = "restored";
constexpr auto kStepTrashed = "trashed";

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
            if (record.memberId == member.memberId &&
                record.lastDurableStep == QLatin1String(kStepStaged)) {
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

    const QString path = QDir(directory).absoluteFilePath(StagingExecutor::manifestFileName());
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
    for (const QJsonValue& value : entries) {
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

        // Membership must still match what was reviewed. A missing previously
        // known RAW blocks the whole group.
        QHash<QString, const domain::FileMember*> currentByPath;
        for (const domain::FileMember& member : asset->members) {
            currentByPath.insert(member.absolutePath, &member);
        }

        QString blocker;
        for (const PlannedMember& member : group.members) {
            const domain::FileMember* found = currentByPath.value(member.sourcePath, nullptr);
            if (found == nullptr) {
                blocker = tr("'%1' is no longer part of this photo.").arg(member.fileName);
                break;
            }
            const domain::FileFingerprint actual = fingerprintOf(member.sourcePath);
            if (!actual.isKnown()) {
                blocker = tr("'%1' no longer exists.").arg(member.fileName);
                break;
            }
            if (actual.sizeBytes != member.expected.sizeBytes ||
                actual.modifiedMsecsUtc != member.expected.modifiedMsecsUtc) {
                blocker = tr("'%1' changed on disk since this operation was reviewed.")
                              .arg(member.fileName);
                break;
            }
            // Rename-only staging cannot cross a filesystem boundary. The
            // spec asks for this to block the policy rather than fail at
            // member N with half the group already moved.
            if (stagingDevice.has_value()) {
                const std::optional<quint64> sourceDevice = deviceOf(member.sourcePath);
                if (!sourceDevice.has_value() || *sourceDevice != *stagingDevice) {
                    blocker = tr("'%1' is on a different filesystem from the staging directory "
                                 "'%2'. This deletion policy moves files by rename only, so it "
                                 "cannot move this photo.")
                                  .arg(member.fileName, plan.stagingRoot);
                    break;
                }
            }
        }
        if (blocker.isEmpty() && group.members.size() != asset->members.size()) {
            blocker = tr("This photo now has a different number of files than was reviewed.");
        }

        // Re-enumerate the directory: a companion that appeared after the
        // last scan is not in the plan, and moving the rest would orphan it.
        if (blocker.isEmpty() && !group.members.isEmpty()) {
            QSet<QString> planned;
            for (const PlannedMember& member : group.members) {
                planned.insert(QFileInfo(member.sourcePath).absoluteFilePath());
            }
            const QFileInfo firstMember(group.members.first().sourcePath);
            const QString stem = firstMember.completeBaseName();
            const QFileInfoList siblings = firstMember.dir().entryInfoList(
                QDir::Files | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo& sibling : siblings) {
                // The same policy as the scan: a switched-off sidecar is not
                // part of the photo and never was, so it cannot be left
                // behind; an unrecognised same-stem file, on the other hand,
                // is exactly what blocks a group.
                if (config_.isDisabledSidecar(sibling.suffix())) {
                    continue;
                }
                if (sibling.completeBaseName() == stem &&
                    !planned.contains(sibling.absoluteFilePath())) {
                    blocker = tr("'%1' appeared beside this photo after it was reviewed.")
                                  .arg(sibling.fileName());
                    break;
                }
            }
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
    for (const PlannedMember& member : group.members) {
        const int index = indexOfMember(record, member.memberId);
        if (index >= 0) {
            record.members[index].stagingPath = QDir(directory).absoluteFilePath(member.fileName);
            record.members[index].lastDurableStep = QLatin1String(kStepPlanned);
        }
    }
    QString journalError;
    if (!writeJournal(&journalError)) {
        return withState(record, OperationState::Failed, journalError); // Nothing has moved.
    }

    QList<PlannedMember> movedMembers;
    for (const PlannedMember& member : group.members) {
        const QString destination = QDir(directory).absoluteFilePath(member.fileName);
        QString moveError;
        if (!renameOnly(member.sourcePath, destination, &moveError)) {
            // Stop the group and attempt journalled restoration of what moved.
            QStringList restoreProblems;
            for (const PlannedMember& moved : movedMembers) {
                const QString staged = QDir(directory).absoluteFilePath(moved.fileName);
                QString restoreError;
                if (!renameOnly(staged, moved.sourcePath, &restoreError)) {
                    // Never overwrite: the staging copy is retained and shown
                    // as a recovery task.
                    restoreProblems.append(restoreError);
                } else {
                    const int index = indexOfMember(record, moved.memberId);
                    if (index >= 0) {
                        record.members[index].stagingPath.clear();
                        record.members[index].lastDurableStep = QLatin1String(kStepRestored);
                    }
                }
            }

            const int index = indexOfMember(record, member.memberId);
            if (index >= 0) {
                record.members[index].error = moveError;
            }

            if (restoreProblems.isEmpty()) {
                return withState(record, OperationState::Failed, moveError);
            }
            return withState(record, OperationState::NeedsRecovery,
                             tr("%1 Some files could not be put back: %2")
                                 .arg(moveError, restoreProblems.join(QLatin1String(" "))));
        }

        const int index = indexOfMember(record, member.memberId);
        if (index >= 0) {
            record.members[index].stagingPath = destination;
            record.members[index].lastDurableStep = QLatin1String(kStepStaged);
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
    for (const PlannedMember& member : group.members) {
        const QString staged = QDir(directory).absoluteFilePath(member.fileName);
        const domain::FileFingerprint actual = fingerprintOf(staged);
        if (!actual.isKnown() || actual.sizeBytes != member.expected.sizeBytes) {
            return withState(record, OperationState::NeedsRecovery,
                             tr("'%1' did not arrive in staging as expected. The files are kept "
                                "for recovery and nothing was sent to Trash.")
                                 .arg(member.fileName));
        }
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
    // outcome, never something to repeat.
    record.state = OperationState::Trashing;
    if (!writeJournal(&journalError)) {
        return withState(record, OperationState::NeedsRecovery, journalError);
    }

    QString trashPath;
    QString trashError;
    if (!trash_.moveToTrash(directory, &trashPath, &trashError)) {
        // The complete group is retained. Retry Trash or Restore are the only
        // options; there is never an automatic fall back to permanent deletion.
        return withState(record, OperationState::NeedsRecovery,
                         tr("The photo was staged but could not be moved to Trash: %1 The "
                            "complete group is kept in '%2'; retry Trash or restore it.")
                             .arg(trashError, directory));
    }

    // The platform need not report a Trash path, so recovery relies on the
    // manifest inside the group directory rather than on this value.
    if (record.trashPath.isEmpty()) {
        record.trashPath = trashPath;
    }
    for (const PlannedMember& member : group.members) {
        const int index = indexOfMember(record, member.memberId);
        if (index >= 0) {
            record.members[index].lastDurableStep = QLatin1String(kStepTrashed);
        }
    }

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
        const QString directory =
            QDir(record.plan.stagingRoot).absoluteFilePath(group.stagingDirectoryName);

        // Members the journal already saw reach Trash need nothing.
        bool journalSaysTrashed = !group.members.isEmpty();
        for (const PlannedMember& member : group.members) {
            const int index = indexOfMember(record, member.memberId);
            if (index < 0 ||
                record.members.at(index).lastDurableStep != QLatin1String(kStepTrashed)) {
                journalSaysTrashed = false;
                break;
            }
        }
        if (journalSaysTrashed) {
            ++trashedGroups;
            continue;
        }

        // The manifest beside the files is consulted before the journal's
        // paths are trusted: a crash between a rename and its confirmation
        // leaves the journal one step behind, and the manifest, or failing
        // that the plan, still says where the file was going.
        QString manifestError;
        const std::optional<Manifest> manifest = readManifest(directory, &manifestError);

        QStringList missing;
        bool anyFound = false;
        int restoredHere = 0;
        for (const PlannedMember& member : group.members) {
            const int index = indexOfMember(record, member.memberId);
            if (index < 0) {
                continue;
            }
            OperationMemberRecord& entry = record.members[index];

            QString staged = entry.stagingPath;
            if (staged.isEmpty() && manifest.has_value()) {
                const ManifestEntry* known = manifest->entry(member.memberId);
                if (known != nullptr && !known->stagingPath.isEmpty()) {
                    staged = known->stagingPath;
                }
            }
            if (staged.isEmpty()) {
                staged = QDir(directory).absoluteFilePath(member.fileName);
            }

            const bool sourceExists = QFileInfo::exists(entry.sourcePath);
            const bool stagedExists = QFileInfo::exists(staged);
            if (sourceExists && !stagedExists) {
                // Never moved, or already put back.
                anyFound = true;
                entry.stagingPath.clear();
                entry.lastDurableStep = QLatin1String(kStepPlanned);
                continue;
            }
            if (!stagedExists) {
                missing.append(member.fileName);
                continue;
            }
            anyFound = true;
            if (sourceExists) {
                problems.append(tr("'%1' exists both at its original path and in staging; "
                                   "nothing was overwritten.")
                                    .arg(member.fileName));
                ++stillStaged;
                continue;
            }

            // A file in staging is only put back when it is the file that was
            // reviewed. Existence alone proves nothing after a crash.
            if (const domain::FileFingerprint actual = fingerprintOf(staged);
                !actual.isKnown() || actual.sizeBytes != member.expected.sizeBytes) {
                problems.append(tr("'%1' is in staging but is not the file that was reviewed "
                                   "(its size differs); it was left where it is.")
                                    .arg(member.fileName));
                entry.stagingPath = staged;
                ++stillStaged;
                continue;
            }

            if (QString moveError; !renameOnly(staged, entry.sourcePath, &moveError)) {
                problems.append(moveError);
                entry.stagingPath = staged;
                ++stillStaged;
                continue;
            }
            entry.stagingPath.clear();
            entry.lastDurableStep = QLatin1String(kStepRestored);
            ++restoredHere;
        }

        if (!missing.isEmpty()) {
            if (!anyFound && !QFileInfo::exists(directory)) {
                // The whole group vanished together, which is what a Trash
                // of the group directory looks like. Confirmed only when the
                // reported Trash location still holds this group's manifest;
                // otherwise the outcome is recorded as uncertain and left to
                // a person. Nothing is ever deleted again on a guess.
                std::optional<Manifest> inTrash;
                if (!record.trashPath.isEmpty()) {
                    QString ignored;
                    inTrash = readManifest(record.trashPath, &ignored);
                }
                if (inTrash.has_value() && inTrash->assetId == group.assetId) {
                    for (const PlannedMember& member : group.members) {
                        const int index = indexOfMember(record, member.memberId);
                        if (index >= 0) {
                            record.members[index].lastDurableStep = QLatin1String(kStepTrashed);
                        }
                    }
                    ++trashedGroups;
                } else {
                    problems.append(
                        tr("'%1' (%2 file(s)) is in neither its original location nor staging. "
                           "It was probably moved to Trash before that could be recorded; check "
                           "the Trash before doing anything else. Nothing will be deleted again.")
                            .arg(group.displayName)
                            .arg(group.members.size()));
                    for (const PlannedMember& member : group.members) {
                        const int index = indexOfMember(record, member.memberId);
                        if (index >= 0) {
                            record.members[index].error =
                                tr("Not found in staging or at the original path.");
                        }
                    }
                }
            } else {
                // Part of the group is accounted for and part is not. Do not
                // assume the last recorded step completed: this needs a person.
                for (const QString& fileName : missing) {
                    problems.append(tr("'%1' is in neither its original nor its staging location.")
                                        .arg(fileName));
                }
            }
        } else if (restoredHere > 0 || anyFound) {
            ++restoredGroups;
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

} // namespace cullfinch::infrastructure
