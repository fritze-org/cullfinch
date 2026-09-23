// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <span>

/// Startup-time adjustments to how the desktop environment is used. These are
/// decisions about the *environment* Cullfinch runs in, not about Cullfinch,
/// which is why they live beside the composition root rather than in a widget.
///
/// Each effect is split from the policy it applies: the policy is a pure
/// function that a test can state the rule to, and the effect is the one-line
/// application of it that main() cannot be handed a double for.
namespace cullfinch::app {

/// The value QT_QPA_PLATFORMTHEME should carry, given what it carries now.
///
/// A null QByteArray means: leave it alone. An explicit setting is the user's
/// choice, and an empty one is the case this exists for.
[[nodiscard]] QByteArray platformThemePreference(const QByteArray& configured);

/// Ask Qt for portal-backed native dialogs when nothing better is configured.
///
/// This vcpkg Qt ships exactly one platform theme plugin, xdgdesktopportal --
/// there is no Breeze or GTK plugin to load. On a KDE session Qt still picks
/// its built-in QKdeTheme, which supplies colours and fonts but no native
/// dialogs, and so never falls through to the portal. QFileDialog then builds
/// its own widget dialog, and that dialog looks up themed icons for its
/// toolbar, its sidebar and every mime type in the listing.
///
/// Each icon that misses is probed against every directory every installed
/// icon theme declares. That is cheap until one of those themes sits on a slow
/// mount, at which point opening a directory takes seconds; see
/// isUnreachableIconThemePath() for the case that found this. Routing the
/// dialog through the portal avoids the scan entirely, because the dialog is
/// drawn by the portal process and not by us: on the machine this was measured
/// on it cut the probes from 138722 to 189, and the wait from about six
/// seconds to none.
///
/// Must run before the QApplication that reads the variable. If no portal is
/// running the theme reports no file-dialog support and Qt falls back to the
/// widget dialog, so this is a preference, not a requirement. Linux only.
void preferPortalDialogs();

/// Whether an icon theme search path belongs to another program's AppImage.
///
/// AppImages mount themselves at <temporary directory>/.mount_<name><random>,
/// and some put that mount at the front of XDG_DATA_DIRS. Every GUI program
/// started from such an AppImage -- a terminal emulator, most often --
/// inherits it, and Qt then treats the bundle's icon theme as a system theme.
/// A bundle that ships eleven icons behind an index.theme declaring 649
/// directories turns a single icon miss into 649 probes across a compressed
/// FUSE mount.
///
/// Such a path is never ours to search: the icons in it belong to the program
/// holding the mount open, and the mount disappears when that program exits.
[[nodiscard]] bool isUnreachableIconThemePath(const QString& path);

/// The subset of `searchPaths` that is ours to search, in the original order.
[[nodiscard]] QStringList reachableIconThemePaths(const QStringList& searchPaths);

/// Drop the unreachable entries from QIcon's icon theme search paths.
///
/// Cullfinch itself draws no themed icons, so only the fallback file dialog is
/// affected -- but that is the dialog people open first. Must run after the
/// QApplication that populates the paths. Linux only.
void pruneUnreachableIconThemePaths();

/// The platform the command line asks Qt for, or a null QString if it asks for none.
///
/// Read the way QGuiApplication reads it: `-platform <spec>` or `--platform <spec>`, the last one
/// winning, a trailing `-platform` with nothing after it ignored, and the value of Qt's other
/// options -- `-platformtheme -platform xcb` sets a theme -- never mistaken for one. Must run
/// before the QApplication is constructed, because Qt removes the option from argv as it consumes
/// it.
[[nodiscard]] QString platformRequestedOnCommandLine(std::span<char* const> arguments);

/// Whether the platform backend Qt selected is a fallback worth warning about.
///
/// A Wayland session that ends up on XCB through XWayland looks identical to a working native run
/// until something subtle misbehaves (decision 0007), so that is reported. A backend the user
/// named is theirs to choose and is not. The command line overrides QT_QPA_PLATFORM, as it does
/// in Qt.
///
/// This takes the requested *value* and not merely whether one was given. A request is a
/// `;`-separated fallback list, and `wayland;xcb` landing on `xcb` is exactly the silent fallback
/// this exists to report. So only a first entry naming the backend in use counts as a choice.
[[nodiscard]] bool platformFallbackDeservesWarning(const QString& backend, bool waylandSession,
                                                   const QString& commandLinePlatform,
                                                   const QString& environmentPlatform);

/// Report the backend actually in use, and say so plainly when it is not the
/// one this platform is built around.
///
/// A Wayland session that silently ends up on XCB through XWayland looks
/// identical to a working native run until something subtle misbehaves, so the
/// fallback is diagnosed rather than hidden. It is reported, never overridden,
/// and a backend the user named is not reported at all; see
/// platformFallbackDeservesWarning().
///
/// @param program the executable's name, as the messages should call it.
/// @param commandLinePlatform platformRequestedOnCommandLine(), read before the
///        QApplication consumed it.
void reportPlatformBackend(const QString& program, const QString& version,
                           const QString& commandLinePlatform);

} // namespace cullfinch::app
