// SPDX-License-Identifier: GPL-3.0-or-later
#include <cullfinch/infrastructure/StagingExecutor.h>

#include <cullfinch/infrastructure/Paths.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

#include <filesystem>
#include <system_error>

namespace cullfinch::infrastructure {
namespace {

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
            if (record.memberId == member.memberId) {
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

} // namespace

QString StagingExecutor::manifestFileName() {
    return QStringLiteral("cullfinch-manifest.json");
}

StagingExecutor::StagingExecutor(application::ITrashAdapter& trash) : trash_(trash) {}

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
        }
        if (blocker.isEmpty() && group.members.size() != asset->members.size()) {
            blocker = tr("This photo now has a different number of files than was reviewed.");
        }
        if (!blocker.isEmpty()) {
            result.blocked.append(domain::PlanningIssue{group.assetId, group.displayName, blocker});
        }
    }

    return result;
}

OperationRecord StagingExecutor::executeGroup(const OperationRecord& input,
                                              const PlannedGroup& group) {
    OperationRecord record = input;
    record.state = OperationState::Staging;

    const QString directory =
        QDir(record.plan.stagingRoot).absoluteFilePath(group.stagingDirectoryName);
    if (!QDir().mkpath(directory)) {
        return withState(record, OperationState::Failed,
                         tr("The staging directory '%1' could not be created.").arg(directory));
    }

    const auto memberIndex = [&record](const domain::MemberId& id) {
        for (int i = 0; i < record.members.size(); ++i) {
            if (record.members.at(i).memberId == id) {
                return i;
            }
        }
        return -1;
    };

    // The manifest is written before anything moves, so an interrupted run is
    // always explainable.
    QString manifestError;
    if (writeManifest(group, directory, record.members, &manifestError).isEmpty()) {
        return withState(record, OperationState::Failed, manifestError);
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
                    const int index = memberIndex(moved.memberId);
                    if (index >= 0) {
                        record.members[index].stagingPath.clear();
                        record.members[index].lastDurableStep = QLatin1String(kStepRestored);
                    }
                }
            }

            const int index = memberIndex(member.memberId);
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

        const int index = memberIndex(member.memberId);
        if (index >= 0) {
            record.members[index].stagingPath = destination;
            record.members[index].lastDurableStep = QLatin1String(kStepStaged);
        }
        movedMembers.append(member);
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

    record.state = OperationState::Trashing;

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
        const int index = memberIndex(member.memberId);
        if (index >= 0) {
            record.members[index].lastDurableStep = QLatin1String(kStepTrashed);
        }
    }

    // The operation as a whole is only completed once every group is done; the
    // controller makes that call.
    return withState(record, OperationState::Trashing, QString());
}

OperationRecord StagingExecutor::recover(const OperationRecord& input) {
    OperationRecord record = input;
    record.state = OperationState::Restoring;

    QStringList problems;
    int restored = 0;
    int stillStaged = 0;

    for (int index = 0; index < record.members.size(); ++index) {
        OperationMemberRecord& member = record.members[index];
        const bool sourceExists = QFileInfo::exists(member.sourcePath);
        const bool stagedExists =
            !member.stagingPath.isEmpty() && QFileInfo::exists(member.stagingPath);

        if (sourceExists && !stagedExists) {
            member.lastDurableStep = QLatin1String(kStepPlanned);
            continue; // Never moved, or already put back.
        }
        if (!stagedExists) {
            // Neither location holds the file. Do not assume the last recorded
            // step completed: this needs a person.
            problems.append(tr("'%1' is in neither its original nor its staging location.")
                                .arg(QFileInfo(member.sourcePath).fileName()));
            continue;
        }
        if (sourceExists) {
            problems.append(tr("'%1' exists both at its original path and in staging; nothing "
                               "was overwritten.")
                                .arg(QFileInfo(member.sourcePath).fileName()));
            ++stillStaged;
            continue;
        }

        QString moveError;
        if (!renameOnly(member.stagingPath, member.sourcePath, &moveError)) {
            problems.append(moveError);
            ++stillStaged;
            continue;
        }
        member.stagingPath.clear();
        member.lastDurableStep = QLatin1String(kStepRestored);
        ++restored;
    }

    if (!problems.isEmpty() || stillStaged > 0) {
        return withState(record, OperationState::NeedsRecovery, problems.join(QLatin1String(" ")));
    }
    // Everything is back at its original path, so the reviewed plan is intact
    // and may be retried. Reporting this as "completed" would falsely imply the
    // photos reached Trash.
    Q_UNUSED(restored)
    return withState(record, OperationState::Planned, QString());
}

} // namespace cullfinch::infrastructure
