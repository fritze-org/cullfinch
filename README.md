# Cullfinch

Cull a directory of photos: compare candidates two at a time or all at once, mark the rejects,
and move each rejected photo to Trash **as a complete group** — the JPG and every RAW file that
belongs with it.

[![CI](https://github.com/fritze-org/cullfinch/actions/workflows/ci.yml/badge.svg)](https://github.com/fritze-org/cullfinch/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/fritze-org/cullfinch/branch/main/graph/badge.svg)](https://codecov.io/gh/fritze-org/cullfinch)

> Status: early development. The architecture, build system, tooling and test suites are in
> place and the two comparison flows work end to end. Nothing here has been through a release.

## What it does

- **Browse a directory.** One thumbnail per photo, even when `DSCF0123.JPG` and `DSCF0123.RAF`
  are two files. RAW-only, ambiguous and changed groups are visible and explained rather than
  hidden.
- **Compare a selection.** The **versus tree** shows two candidates at a time in a balanced
  bracket until one remains. The **image wall** shows every selected photo at once, fullscreen
  if you like, and removes the ones you click.
- **Eliminating is not deleting.** A comparison only builds a draft. Finishing it turns the
  decisions into collection-level deletion marks, which are undoable.
- **Deleting is a separate, reviewed step.** *Review file operations* lists every physical file,
  its size and any blocker before anything moves. A missing RAW blocks its whole group.

## Requirements

- Linux x86-64, primarily on **native Wayland**, with X11 as a compatibility backend
- macOS (Apple Silicon or Intel)
- A C++20 compiler, CMake 3.28+, Ninja and Git

Qt comes from vcpkg, which CMake bootstraps for you. You do **not** need a system Qt, and
Cullfinch will never use one it finds by accident.

On Linux, Cullfinch runs as a native Wayland client in a Wayland session and reports the
platform plugin it actually got at startup. It never silently falls back to XWayland: if Qt
picks XCB in a Wayland session you get a warning saying so. `-platform xcb` remains a supported
compatibility option, and `-platform wayland` requires the native path.

## Build

```sh
./scripts/setup-linux.sh      # or ./scripts/setup-macos.sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

The first configure fetches the pinned vcpkg checkout and builds Qt from source, which takes a
while. Later configures reuse both.

Useful knobs:

| Setting | Effect |
|---|---|
| `--preset dev-fast` | Debug build with clang-tidy off, for rapid iteration |
| `-DCULLFINCH_VCPKG_ROOT=…` | Use a vcpkg checkout you already have; it is validated, never modified |
| `-DCULLFINCH_OFFLINE=ON` | Forbid all network access during acquisition and dependency installation |
| `-DCULLFINCH_VCPKG_CACHE=…` | Where the managed, commit-keyed vcpkg checkout lives |

## Develop

```sh
make hooks     # install the pinned tooling and the Git hooks
make lint      # the complete pre-commit suite over every tracked file
make tidy      # a fresh clang-tidy analysis build
make coverage  # instrumented build, all three suites, Cobertura + HTML report
```

`make` targets are thin wrappers around the documented CMake presets; CMake remains the build.

Development tools (pre-commit, gcovr, ruff, shellcheck, actionlint) are pinned through `uv` in
`pyproject.toml` and `uv.lock`. That adds no Python dependency to the application.

`.pre-commit-config.yaml` pins each hook to a commit SHA with the tag kept in a trailing
comment. Update hook versions with `pre-commit autoupdate --freeze`, then confirm
`pre-commit validate-config` still passes; never hand-edit a `rev:` back to a bare tag.

## Tests

| Suite | Label | What it covers |
|---|---|---|
| Unit | `unit` | Pairing rules, bracket construction, flow transitions, layout geometry, selection snapshots, mark merging, operation planning |
| Integration | `integration` | Real temporary directories, real SQLite, staging and recovery under injected faults |
| GUI | `gui` | The production widgets and composition, driven through Qt Test with injected adapters |
| Package | `package` | The installed application in a clean environment, proving the deployed plugins load |

```sh
ctest --preset dev --label-regex unit
```

GUI tests use whatever `QT_QPA_PLATFORM` you give them — `offscreen` by default on a headless
machine, `cocoa` on macOS. Two helpers own a complete isolated session, including their own
compositor, D-Bus and runtime directory:

```sh
tests/support/with-wayland.sh ctest --preset dev-fast --label-regex gui   # required backend
tests/support/with-x11.sh     ctest --preset dev-fast --label-regex gui   # compatibility
```

Both set `CULLFINCH_EXPECTED_PLATFORM`, so a run that quietly lands on XWayland, XCB or
offscreen fails instead of passing under a backend nobody asked for. The Wayland helper waits
for a real client connection rather than for a socket to appear, and unsets `DISPLAY` so
XWayland cannot rescue a broken native path.

## Branch protection

The repository's protection rules live in [`.github/rulesets/`](.github/rulesets/) as GitHub
ruleset definitions, so what protects `main` is reviewed like everything else:

| Ruleset | Applies to | What it enforces |
|---|---|---|
| [`main.json`](.github/rulesets/main.json) | the default branch | No deletion, no force-push; changes arrive through a pull request with every review thread resolved; the `Required checks` aggregation job and the SonarCloud quality gate must pass on a head that is up to date with `main`. Repository admins may bypass only through a pull request, never by pushing directly. |
| [`release-tags.json`](.github/rulesets/release-tags.json) | `v*` tags | A release tag can neither be moved nor deleted once it exists. |

`Required checks` is the one job that depends on every required CI job (pre-commit on both
platforms, every build-and-test entry, clang-tidy and coverage), so it is the single Actions
context a ruleset needs to name; `codecov/patch` stays informational, as `codecov.yml` records.
Each required check is bound to the GitHub App that reports it (`integration_id` 15368 for
Actions, 12526 for SonarCloud), so a status with the same name from any other source does not
satisfy it.
The main ruleset requires no approving review because the project currently has one maintainer;
raise `required_approving_review_count` to 1 once a second maintainer can review.

To apply or update them: *Settings → Rules → Rulesets → New ruleset → Import a ruleset*, and
choose the file. Importing needs repository admin rights, which is why this is a checked-in
definition rather than something CI can do.

## Layout

```text
src/domain/          identities, assets, pairing policy, flow contracts   (no widgets, no SQL)
src/application/     session lifecycle, validation, marks, operation planning
src/flows/versus/    bracket state and transitions
src/flows/wall/      candidate-set state, transitions and layout policy
src/infrastructure/  scanning, images, SQLite, staging, Trash adapter
src/ui/              browser, shared comparison shell, image widgets
src/views/*/         per-flow presentation
src/app/             composition root and startup
```

Adding a comparison mode means writing an engine, writing its view, registering both under one
stable identifier in `src/app/CompositionRoot.cpp`, and adding tests. No `switch` in the
browser, the pairing resolver or the operation executor needs to change; `tests/unit/
ConformanceFlow.h` is the fixture that keeps that claim honest.

## Documentation

- [`docs/design.md`](docs/design.md) — the full design specification this implements
- [`docs/decisions/`](docs/decisions/) — accepted decisions, including where the implementation
  deviates from the specification and why

## Known limitations

- Colour handling converts embedded profiles to sRGB and assumes sRGB for untagged files. There
  is no monitor-profile discovery, so this is not calibrated output.
- Undo history for comparisons lives only within a run. Saved drafts and deletion marks survive
  a restart; the undo stack does not.
- A completed group appears in Trash as a *directory*. Restoring it from the desktop does not
  restore the original individual file paths — the manifest inside it exists so Cullfinch can.
- Deletion requires a writable staging location on the same filesystem as the photos, and all of
  a group's files must be on one filesystem.
- RAW files are opaque companions. Cullfinch never decodes them and makes no claim to support
  any RAW format's contents.
- One Cullfinch instance writes to a collection at a time. A second instance that opens the same
  directory gets it read-only: it shows the inventory as last stored, does not rescan, and refuses
  marks, comparisons and file operations until the first instance closes the collection.
- Fullscreen restores the window's previous *size* and state, not its exact desktop position:
  Wayland does not let a client place its own window, and `QWindow::setPosition()` is documented
  as unsupported there.
- 100% inspection means one source-image pixel per rendered *buffer* pixel. Under fractional
  scaling a compositor may resample the surface afterwards, so a guaranteed one-to-one mapping
  to physical panel pixels is not promised.
- Flatpak packaging is not provided. An ordinary AppImage is not sandboxed, so the local
  filesystem, RAW association and Trash design apply as described; a portal-sandboxed build
  would need its directory access, companion-file discovery and staging directory verified
  separately.

## Licence

[GPL-3.0-or-later](LICENSE). cullfinch links Qt 6 dynamically under the LGPLv3.
