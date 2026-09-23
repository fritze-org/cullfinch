# 0011 — The standalone wall viewer is a second executable that decides nothing

Status: accepted · Date: 2026-09-23

## Context

The image wall is useful on its own, as a way to look at a folder: every photo at once, in a grid
that fills the screen. Opening Cullfinch for that means a database, a collection lock, a selection
and a comparison session, all for a task that changes nothing.

The wall's layers were already separate enough to reuse without any of that. `WallLayout` is pure
geometry. `WallSurface` renders wall positions and emits an elimination request that it never acts
on itself. `ImageCanvas`, `PreviewLoader` and `QtImageService` do the decoding.
`DirectoryScanner::enumerate` and `StemAssociationResolver` turn a directory into photos exactly as
the browser sees them.

What the comparison wall cannot offer is a tile size. [Design §7](../design.md) makes it fit every
candidate on one screen, because a photo scrolled out of sight is not being compared. It mentions a
"user-selected scrollable enlarged view" only as something to offer alongside that.

## Decision

`cullfinch-wall` is a second executable in this repository, built from `src/viewer/` and installed
next to `cullfinch`. It reuses the libraries above and owns nothing that persists:

- **No flow and no session.** Nothing is connected to the surface's elimination request, so a click
  or Delete on a tile does nothing. Space still inspects at 100%.
- **No database, no lock, no file operation.** It cannot change a file.
- **A tile width.** `WallLayoutOptions::cellWidth` switches the layout from fit-to-viewport to rows
  of cells about that wide that grow downwards. The surface sits in a `QScrollArea`. Zero keeps
  today's fit-all layout, and the comparison wall never sets anything else, so its behaviour and its
  visual references are unchanged.
- **Decoding follows the screen.** With a tile width set, tiles hold back their decode
  (`ImageCanvas::setLoadingDeferred`) until they come within one screen of the viewport. While a
  preview is already on screen, a resized tile waits for the size to settle
  (`ImageCanvas::setRefinementDelay`) before decoding at the new size. Both default to the old
  behaviour.
- **Shared startup code.** Portal dialogs, icon-theme pruning and the platform-backend report move
  from `src/app/main.cpp` into a small `cullfinch_desktop` library, so both executables diagnose an
  XWayland fallback the same way ([decision 0007](0007-wayland-primary-linux-backend.md)).

## Consequences

- The package ships two binaries and two `.desktop` entries. The packaged-application suite finds
  the viewer beside the application and runs its `--version` and `--smoke` paths, so a package that
  leaves the viewer out fails that suite.
- The AppImage's `AppRun` still starts `cullfinch`; the viewer is inside the image but has no entry
  point there of its own. The macOS disk image carries a second bundle.
- The viewer links `cullfinch_infrastructure` for the scanner and the image service, and through it
  QtSql, which it never uses. Splitting imaging and scanning out of that library would remove the
  dependency. It is not needed for correctness.
- Fixed-width tiles use the placeholder aspect (3:2), so portrait photos are letterboxed inside
  landscape cells, as they already are on the comparison wall.
