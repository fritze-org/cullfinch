# One CTest entry per independent test executable, labelled so the presets and CI jobs can select
# suites. Registration never inspects the display: the Qt platform plugin is chosen through the test
# environment instead.
add_subdirectory(support)

# ------------------------------------------------------------------- unit ----
# No display required. These cover pairing, bracket construction, flow transitions, layout geometry,
# selection snapshots, mark merging and operation planning.
cullfinch_add_test(
  tst_natural_order
  SOURCES unit/tst_natural_order.cpp
  DEPS cullfinch::cullfinch_domain
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_association_policy
  SOURCES unit/tst_association_policy.cpp
  DEPS cullfinch::cullfinch_domain
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_selection_snapshot
  SOURCES unit/tst_selection_snapshot.cpp
  DEPS cullfinch::cullfinch_domain cullfinch::cullfinch_testsupport
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_operation_planner
  SOURCES unit/tst_operation_planner.cpp
  DEPS cullfinch::cullfinch_domain cullfinch::cullfinch_testsupport
  LABELS unit PLATFORM offscreen)

# The offer logic on its own: what a stopped operation's journal allows a person to do is decided
# without a filesystem, so it is pinned down without one.
cullfinch_add_test(
  tst_recovery_offers
  SOURCES unit/tst_recovery_offers.cpp
  DEPS cullfinch::cullfinch_application
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_versus_flow
  SOURCES unit/tst_versus_flow.cpp
  DEPS cullfinch::cullfinch_flow_versus
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_wall_flow
  SOURCES unit/tst_wall_flow.cpp
  DEPS cullfinch::cullfinch_flow_wall
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_wall_layout
  SOURCES unit/tst_wall_layout.cpp
  DEPS cullfinch::cullfinch_flow_wall
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_flow_registry
  SOURCES unit/tst_flow_registry.cpp unit/ConformanceFlow.h
  DEPS cullfinch::cullfinch_application cullfinch::cullfinch_flow_versus
       cullfinch::cullfinch_flow_wall
  LABELS unit PLATFORM offscreen)

cullfinch_add_test(
  tst_session_controller
  SOURCES unit/tst_session_controller.cpp unit/ConformanceFlow.h
  DEPS cullfinch::cullfinch_application cullfinch::cullfinch_flow_versus
       cullfinch::cullfinch_testsupport
  LABELS unit PLATFORM offscreen
  TIMEOUT 180)

# The decode bookkeeping behind a canvas, without a canvas: which request is current, which result
# belongs to the photo on screen, and when a host is told a preview is ready. Readiness gates
# decision input, so getting it wrong is a correctness bug in the flows rather than a cosmetic one.
cullfinch_add_test(
  tst_preview_loader
  SOURCES unit/tst_preview_loader.cpp
  DEPS cullfinch::cullfinch_ui cullfinch::cullfinch_testsupport
  LABELS unit PLATFORM offscreen)

# Startup-time desktop integration is a policy decision plus a one-line effect, and both are checked
# here: the effects are process-wide environment and QIcon state, which this restores.
cullfinch_add_test(
  tst_desktop_integration
  SOURCES unit/tst_desktop_integration.cpp
  DEPS cullfinch::cullfinch_composition
  LABELS unit PLATFORM offscreen)

# ------------------------------------------------------------ integration ----
# Real temporary directories and a real SQLite database, with controllable adapters for the failure
# paths.
cullfinch_add_test(
  tst_directory_scanner
  SOURCES integration/tst_directory_scanner.cpp
  DEPS cullfinch::cullfinch_infrastructure cullfinch::cullfinch_testsupport
  LABELS integration PLATFORM offscreen
  TIMEOUT 300)

cullfinch_add_test(
  tst_sqlite_repository
  SOURCES integration/tst_sqlite_repository.cpp
  DEPS cullfinch::cullfinch_infrastructure cullfinch::cullfinch_testsupport
  LABELS integration PLATFORM offscreen
  TIMEOUT 300)

cullfinch_add_test(
  tst_operation_controller
  SOURCES integration/tst_operation_controller.cpp
  DEPS cullfinch::cullfinch_infrastructure cullfinch::cullfinch_testsupport
  LABELS integration PLATFORM offscreen
  TIMEOUT 300)

cullfinch_add_test(
  tst_app_lock
  SOURCES integration/tst_app_lock.cpp
  DEPS cullfinch::cullfinch_infrastructure
  LABELS integration PLATFORM offscreen
  TIMEOUT 120)

cullfinch_add_test(
  tst_staging_executor
  SOURCES integration/tst_staging_executor.cpp
  DEPS cullfinch::cullfinch_infrastructure cullfinch::cullfinch_testsupport
  LABELS "integration;native-trash-adjacent" PLATFORM offscreen
  TIMEOUT 300)

# The one suite that uses the platform's real Trash, and so the only place infrastructure::
# QtTrashAdapter and the "the reported Trash path still holds the group" assumption are checked
# against an actual operating system rather than against a double that satisfies them by
# construction. Opt-in: it does nothing unless CULLFINCH_NATIVE_TRASH_TESTS=1 is set, because it
# writes into the Trash of whoever runs it. It removes only the paths it was handed back, and never
# enumerates or empties anything else. Its own label, so no preset or job picks it up by accident.
cullfinch_add_test(
  tst_native_trash
  SOURCES integration/tst_native_trash.cpp
  DEPS cullfinch::cullfinch_infrastructure
  LABELS native-trash PLATFORM offscreen
  TIMEOUT 300)

# Recovery after an actual process death, rather than after a journal snapshot replayed in-process.
# The suite spawns cullfinch_crash_helper, which kills itself at a named journal write, and then
# opens the same database. It runs in every row that runs the integration suites, sanitizers
# included: five short-lived children are not what makes that job slow.
cullfinch_add_test(
  tst_crash_recovery
  SOURCES integration/tst_crash_recovery.cpp
  DEPS cullfinch::cullfinch_infrastructure cullfinch::cullfinch_testsupport
  LABELS integration PLATFORM offscreen
  TIMEOUT 300)

# The helper is launched by path, never linked, so the dependency has to be stated: without it a
# fresh build tree races the test against a binary that does not exist yet.
add_dependencies(tst_crash_recovery cullfinch_crash_helper)
target_compile_definitions(tst_crash_recovery
                           PRIVATE CULLFINCH_CRASH_HELPER="$<TARGET_FILE:cullfinch_crash_helper>")

# -------------------------------------------------------------------- gui ----
# The production widgets and the production composition, with injectable adapters. The platform
# plugin comes from the environment so the same binaries run offscreen, under XCB and under Cocoa.
cullfinch_add_test(
  tst_browser_window
  SOURCES gui/tst_browser_window.cpp gui/GuiFixture.h
  DEPS cullfinch::cullfinch_composition cullfinch::cullfinch_testsupport
  LABELS gui
  TIMEOUT 300)

# The canvas on its own, against an image service that answers on command. The view suites drive it
# through a real decoder, which cannot say when a preview arrives or fail one deliberately, and
# those orderings are what decide whether a photo can be eliminated.
cullfinch_add_test(
  tst_image_canvas
  SOURCES gui/tst_image_canvas.cpp
  DEPS cullfinch::cullfinch_ui cullfinch::cullfinch_testsupport
  LABELS gui
  TIMEOUT 120)

cullfinch_add_test(
  tst_versus_view
  SOURCES gui/tst_versus_view.cpp gui/GuiFixture.h
  DEPS cullfinch::cullfinch_composition cullfinch::cullfinch_testsupport
  LABELS gui
  TIMEOUT 300)

cullfinch_add_test(
  tst_wall_view
  SOURCES gui/tst_wall_view.cpp gui/GuiFixture.h
  DEPS cullfinch::cullfinch_composition cullfinch::cullfinch_testsupport
  LABELS gui
  TIMEOUT 300)

# ----------------------------------------------------------------- visual ----
# Renders the production views offscreen and compares the pixels against checked-in references.
# Registered with an explicit platform, unlike the suites above: a compositor decides how a surface
# is finally composed, so a pixel comparison only means something against the backend its reference
# was recorded on. These images are cullfinch's own widget rendering and nothing beyond it.
cullfinch_add_test(
  tst_visual_regression
  SOURCES gui/tst_visual_regression.cpp gui/GuiFixture.h gui/VisualBaseline.h gui/VisualBaseline.cpp
  DEPS cullfinch::cullfinch_composition cullfinch::cullfinch_testsupport
  LABELS visual PLATFORM offscreen
  TIMEOUT 600)

# References are read from, and rewritten in, the source tree: recording a new one then shows up as
# a reviewable diff rather than as an artifact somebody copies into place by hand.
target_compile_definitions(
  tst_visual_regression
  PRIVATE "CULLFINCH_VISUAL_REFERENCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/visual\"")

# The GUI tests deliberately set no QT_QPA_PLATFORM, so they inherit it from the environment:
# offscreen on a headless developer machine, xcb under Xvfb, cocoa on macOS. CI additionally sets
# CULLFINCH_EXPECTED_PLATFORM, which makes an accidental offscreen fallback fail the desktop-backend
# job instead of passing quietly.

# ---------------------------------------------------------------- package ----
# Runs the *installed* application in a clean environment, with no build-tree Qt plugin or library
# paths, and asserts the deployed JPEG, platform and SQLite plugins all load. Skipped unless
# CULLFINCH_PACKAGED_BINARY points at an installed build.
cullfinch_add_test(
  tst_packaged_application
  SOURCES gui/tst_packaged_application.cpp
  DEPS cullfinch::cullfinch_testsupport
  LABELS package PLATFORM offscreen
  TIMEOUT 300)
