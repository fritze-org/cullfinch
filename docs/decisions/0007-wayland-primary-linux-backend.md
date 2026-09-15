# 0007 — Native Wayland is the primary Linux backend

Status: accepted · Date: 2026-09-10 · Supersedes part of [0004](0004-generated-image-fixtures.md)

## Context

Specification revision 0.3 makes native Wayland the primary Linux backend, with
X11 retained as a secondary compatibility path, and confirms the product name
as **Cullfinch** (R17). Revision 0.2 had treated the two display servers as
equals and tested only X11 on every pull request.

## Decision

Cullfinch is a Wayland **client**. Qt supplies the platform integration; there
is no application-specific Wayland protocol code and no compositor dependency.

- The pinned Qt build gains `dbus`, which the portal-backed file chooser and
  desktop integration need.
- `tests/support/with-wayland.sh` owns a complete isolated session — its own
  runtime directory at mode 0700, its own D-Bus, and a headless Weston with the
  pixman software renderer — and `tests/support/with-x11.sh` does the same for
  the compatibility suite. `CULLFINCH_COMPOSITOR` swaps Weston for KWin or
  Mutter and changes nothing else, so the extended compositor jobs vary the one
  thing they are about.
- The complete core GUI suite and the installed-package smoke test run under
  native Wayland on every pull request. X11 is a separate, secondary job.
- The coverage job runs through the same Wayland helper, so the canonical
  report reflects the backend that actually ships.

## Consequences

- **Readiness is a real client connection.** A Wayland socket appears before
  the compositor can serve anyone, so the helper waits for `wayland-info` to
  complete a connection and checks compositor liveness while it waits. It also
  unsets `DISPLAY`, because a job rescued by XWayland has not tested the native
  path at all.
- **Fullscreen no longer restores a position.** `QWindow::setPosition()` is
  unsupported on Wayland, so the shell restores size and window state and
  preserves the comparison state and internal focus. Transitions are handled in
  `changeEvent(WindowStateChange)` rather than assumed to complete
  synchronously, because the compositor decides when they happen.
- **Gestures disarm on interruption.** A focus loss, an activation change, a
  resize or a device-pixel-ratio change cancels an armed rejection. Alt-Tab, a
  workspace switch or the lock screen must never complete a decision about a
  photo the user stopped looking at.
- **100% inspection is defined in buffer pixels**: one source-image pixel per
  rendered buffer pixel, using the *window's* device-pixel ratio rather than
  the screen's, since the two differ under fractional scaling. A compositor may
  still resample the surface afterwards, so a one-to-one mapping to physical
  panel pixels is not claimed.
- **Accessibility improves as a side effect.** Enabling `dbus` restores Qt's
  AT-SPI bridge on Linux, which the v0.2 build had switched off. The limitation
  recorded in the README for revision 0.2 no longer applies.
- Extended CI gains KWin and Mutter compositor jobs and a fractional-scaling
  matrix. Weston stays the deterministic reference compositor for the required
  checks; a full desktop compositor is where decorations, real focus routing
  and fractional scaling are actually exercised.
- **What headless testing does not prove.** QTest injects input in-process, so
  these runs establish Cullfinch's behaviour and its native presentation path,
  not the compositor's real keyboard and pointer routing. A headless compositor
  can lack a physical input seat. X11-only automation such as `xdotool` is
  never used to claim native Wayland coverage.
