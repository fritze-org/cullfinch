// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cullfinch/application/Services.h>

#include <QList>
#include <QString>

#include <optional>

namespace cullfinch::infrastructure {

/// What the recovery manifest inside a staging directory says about one file.
struct ManifestEntry {
    domain::MemberId memberId;
    QString fileName;
    QString originalPath;
    QString stagingPath; ///< Empty until the member has been moved.
    qint64 sizeBytes = -1;
};

/// The recovery manifest: the record that travels with the files, so a group
/// can be put back even when the journal never caught up with the renames.
struct Manifest {
    domain::AssetId assetId;
    QString displayName;
    QList<ManifestEntry> members;

    [[nodiscard]] const ManifestEntry* entry(const domain::MemberId& id) const;
};

/// Recoverable same-filesystem staging, followed by one Trash call on the
/// completed group directory.
///
/// Independent JPG and RAW moves cannot be one filesystem transaction, so this
/// is explicitly recoverable rather than atomically visible across several
/// source files. If it is interrupted, some members may sit in staging while
/// others remain at their original paths. That state is recorded -- in the
/// journal before and after every move, and in a manifest beside the files --
/// reported, and must be repaired before the photo can be operated on again.
///
/// The policy requires a writable staging location on the *source* filesystem,
/// and a completed group appears in Trash as a directory. Restoring that
/// directory through the desktop does not restore the original individual
/// paths; the manifest exists so cullfinch can.
class StagingExecutor final : public application::IOperationExecutor {
public:
    explicit StagingExecutor(application::ITrashAdapter& trash);

    [[nodiscard]] domain::PlanningResult preflight(const domain::OperationPlan& plan,
                                                   const domain::PhotoAssetList& current,
                                                   QString* error) const override;

    application::OperationRecord executeGroup(const application::OperationRecord& record,
                                              const domain::PlannedGroup& group,
                                              const application::JournalWriter& journal) override;

    application::OperationRecord recover(const application::OperationRecord& record) override;

    /// Written into each staging directory before anything moves, so a restore
    /// can put every file back where it came from.
    [[nodiscard]] static QString manifestFileName();

    /// Read the manifest in a staging (or Trash) directory. Absent or
    /// unreadable manifests are reported, never guessed at.
    [[nodiscard]] static std::optional<Manifest> readManifest(const QString& directory,
                                                              QString* error);

private:
    application::ITrashAdapter& trash_;
};

} // namespace cullfinch::infrastructure
