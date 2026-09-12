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

# -------------------------------------------------------------------- gui ----
# The production widgets and the production composition, with injectable adapters. The platform
# plugin comes from the environment so the same binaries run offscreen, under XCB and under Cocoa.
cullfinch_add_test(
  tst_browser_window
  SOURCES gui/tst_browser_window.cpp gui/GuiFixture.h
  DEPS cullfinch::cullfinch_composition cullfinch::cullfinch_testsupport
  LABELS gui
  TIMEOUT 300)

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
