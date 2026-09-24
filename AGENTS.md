# AGENTS.md

Guidance for coding agents working in this repository. The product specification is
[`docs/design.md`](docs/design.md); accepted deviations from it are recorded in
[`docs/decisions/`](docs/decisions/). Read the relevant decision record before changing anything
it covers — several were written because the obvious approach was wrong.

## Commands

```sh
./scripts/setup-linux.sh          # or setup-macos.sh; installs system prerequisites (needs sudo)
cmake --preset dev-fast           # Debug, clang-tidy off — the fast iteration loop
cmake --build --preset dev-fast
ctest --preset dev-fast --output-on-failure
```

The **first** configure fetches a pinned vcpkg checkout and builds Qt from source (40–90 min).
Everything after that reuses it. `--preset dev` is the same but with clang-tidy on, which roughly
doubles build time; use `dev-fast` unless you are specifically checking analysis.

| Task | Command |
|---|---|
| One test executable | `./build/dev-fast/tests/tst_versus_flow` |
| One test case | `./build/dev-fast/tests/tst_wall_view fixedPositionsKeepSurvivorsInPlaceUntilCompact` |
| One suite | `ctest --preset dev-fast --label-regex unit` (`unit`, `integration`, `gui`, `visual`, `package`) |
| Visual regression | `ctest --preset dev-fast --label-regex visual` (`make visual-record` re-records) |
| All hooks | `make lint` — must pass before committing; CI runs the same config on all files |
| Static analysis | `make tidy` (fresh analysis build; an up-to-date tree analyses nothing) |
| Coverage | `make coverage` |

GUI tests need a platform plugin. Either set `QT_QPA_PLATFORM=offscreen`, or use a session helper
that owns a real compositor, its own D-Bus and its own runtime directory:

```sh
tests/support/with-wayland.sh ctest --preset dev-fast --label-regex gui   # primary backend
tests/support/with-x11.sh     ctest --preset dev-fast --label-regex gui   # compatibility

CULLFINCH_COMPOSITOR=kwin tests/support/with-wayland.sh ctest --preset dev-fast --label-regex gui
```

`CULLFINCH_COMPOSITOR` selects the Wayland helper's compositor — `weston` (default), `kwin` or
`mutter`; `with-x11.sh` has no equivalent and always runs Xvfb under XCB. The extended compositor
jobs set only that variable; never hand-roll a compositor launch in a workflow, because a session
assembled inline loses the private bus, the liveness check and the log capture that make a failure
readable.

## Architecture

Layered, with the dependency direction strictly inward. Each CMake target's header comment states
what it must *not* own; those boundaries are the design and are worth preserving.

```text
domain/        identities, PhotoAsset, pairing policy, flow contracts   — no widgets, SQL or I/O
application/   session lifecycle, transition validation, marks, operation planning
flows/*/       pure reducers over their own JSON state                  — no paths, SQL, no other flow
infrastructure/ scanning, image decode, SQLite, staging, Trash
ui/            browser, shared comparison shell, image widgets
views/*/       one view per flow, paired by a stable string id
app/           composition root; the only place that knows every concrete adapter
viewer/        standalone wall viewer (cullfinch-wall): scanner + wall surface, no session
```

Three ideas carry most of the design:

**`PhotoAsset` is the unit, never a file.** One visible photo owns its JPG and every RAW companion.
`pairingState`, `disposition` and `operationsBlocked` are deliberately independent fields — see
[decision 0006](docs/decisions/0006-operations-blocked-flag.md). A symlinked JPG is perfectly fine
to *look at* and unsafe to *move*; collapsing those into one enum loses that.

**Flows are pure reducers.** `IComparisonFlow` takes state plus an action and returns new state plus
a reversible delta. No I/O, no clock, no randomness — that is what makes undo and resume exact.
The state payload is opaque JSON owned by the flow, so the host never needs a
`variant<VersusState, WallState, …>`. Adding a comparison mode means: an engine, a view, one
registration pair in `src/app/CompositionRoot.cpp`, and tests. Nothing else changes.
`tests/unit/ConformanceFlow.h` is the fixture that keeps that claim honest.

**The host distrusts the flow.** `SessionController::validateTransition` rejects any transition that
rejects an unselected photo, loses one, duplicates one, or fails to advance its revision. Actions
carry the state revision the view rendered, so a late click cannot decide a second match.

Deletion is three separate stages on purpose: eliminating updates a session-local draft; finishing
merges it into collection marks; only an explicitly reviewed operation moves files, by staging a
whole group on the source filesystem and then Trashing that directory. A group is never partially
moved without a durable journal recording it.

## Gotchas

These were all found the expensive way. Most cost a full CI round.

- **`slots`, `signals` and `emit` are Qt macros.** `QList<WallSlot> slots(...)` is preprocessed into
  nonsense and produces dozens of cascading syntax errors far from the cause.
- **`QT_NO_CAST_FROM_ASCII` is on.** Use `QStringLiteral` / `QLatin1String`; a bare `"literal"` in a
  `QString` context will not compile.
- **Do not add a file-local `tr()` helper in a `QObject` subclass's translation unit.** Unqualified
  `tr()` binds to the inherited `QObject::tr`, so the helper is dead code and `-Wunused-function`
  fails the `-Werror` jobs. Non-`QObject` translation units genuinely need theirs.
- **A default-constructed `QString` is null, not empty**, and SQLite binds null as SQL `NULL`,
  overriding `DEFAULT ''` on a `NOT NULL` column. `SqliteRepository` has a `text()` helper; use it
  for every text binding. A photo in the scan root has an empty relative directory, so this is the
  common path, not an edge case.
- **CMake's regex dialect has no `{n}` quantifier.** It silently never matches.
- **`CMAKE_HOST_SYSTEM_PROCESSOR` is empty before `project()`**, and every vcpkg variable must be
  set before that. Use `cmake_host_system_information(QUERY OS_PLATFORM)`.
- **A nested aggregate's default member initializers are unusable in a default argument** of the
  enclosing class's constructor. Write two constructors instead.
- **New vcpkg dependencies often need system packages.** vcpkg names them in its error; add them to
  both `scripts/setup-*.sh`. Your machine probably already has them — the cold-cache job in
  `extended.yml` exists because a warm environment passing proves nothing.
- **A headless compositor can have no input seat.** Never assert `hasFocus()` or rely on `setFocus()`
  being honoured in the GUI suites; deliver the event directly instead. Real focus routing belongs
  to the compositor integration suite.
- **`resize()` on a mapped window is a request, and Qt adopts it before the answer arrives.** A
  window manager that already mapped the window at another size can revert it, and `size()` cannot
  tell the two apart; the revert surfaces later as a relayout, moving widgets a test has already
  measured. That is what made the wall suite fail under X11 and nowhere else. GUI tests that read
  geometry use `guitests::settleWindowSize()`, which waits for the size to hold.
- **Wayland cannot place its own windows.** Restore size and window state, never a desktop position,
  and treat fullscreen transitions as asynchronous.
- **`displayName` is a path, not a filename, once a scan is recursive.** It is
  `relativeDirectory + '/' + stem`, so sorting assets by it interleaves directory trees. The
  resolver already returns them ordered by relative directory and then by natural stem, and that
  is the order to show; re-sorting on the name undoes it. Pairing is per directory for the same
  reason, so `a/IMG_1` and `b/IMG_1` are two photos, never one photo with a companion. Only a
  recursive scope sees this — the browser's recursive scan and `cullfinch-wall --recursive`.
- **Releasing a preview's pixels drops its readiness.** `ImageCanvas::releasePixels` exists so a
  host showing far more photos than fit on screen does not keep every image it has scrolled past;
  the photo, its native size and any reported error stay, and only the pixels go. The comparison
  wall and the versus panes gate decision input on readiness, so only a host that decides nothing
  may call it — the standalone wall does, they must not.
- **The image service has no priority queue.** It answers in request order, so a host that
  pre-loads has to pace itself behind what is on screen: a directory handed over at once puts the
  tile under the scrollbar behind a thousand nobody is looking at. `WallSurface` pre-loads only
  while no visible tile is waiting, keeps four decodes outstanding at most, and stops at a share of
  the byte budget, past which another decode only evicts one somebody is closer to needing.
- **`ComparisonShell` takes presentations at construction** because it renders state immediately; a
  view handed an empty map latches onto empty panes.
- **A visual regression reference is an expectation, not an artifact.** Re-recording with
  `CULLFINCH_UPDATE_VISUAL_REFERENCES=1` always makes the suite pass, which is exactly why the
  images must be looked at before they are committed — a recording run cannot tell an intended
  redesign from the regression it was meant to catch. Only pixels the suite masks or pins are
  reproducible; adding a case that renders unmasked text makes it font-dependent, which is what
  the per-environment reference directories are for. See
  [decision 0009](docs/decisions/0009-visual-regression-references.md).
- **Do not `git add -A`.** Spec revisions get dropped in the working tree as inputs; `docs/design.md`
  is the copy the project keeps.

## Conventions

Targets and options use `cullfinch_` / `CULLFINCH_`. Every owned source file carries an
`SPDX-License-Identifier: GPL-3.0-or-later` header. Comments explain *why*, particularly where the
obvious implementation is wrong — that is the house style and reviewers expect it.

Widgets carry stable `objectName`s; GUI tests locate controls by those and assets by `AssetId`,
never by translated label or screen coordinate.
