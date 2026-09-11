# Composition root and startup. It duplicates no business logic; its only job is to choose the
# concrete adapters and register the flows with their views.
#
# The wiring lives in a library so GUI tests can assemble the *production* composition with
# injectable adapters, rather than re-implementing it.
cullfinch_add_library(
  cullfinch_composition
  SOURCES CompositionRoot.cpp
  HEADERS CompositionRoot.h
  PUBLIC_DEPS cullfinch::cullfinch_ui cullfinch::cullfinch_infrastructure
              cullfinch::cullfinch_view_versus cullfinch::cullfinch_view_wall)

target_include_directories(cullfinch_composition PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

qt_add_executable(cullfinch_app main.cpp)

# The raster icon travels inside the binary for QGuiApplication::setWindowIcon(): X11 and the task
# switcher on compatibility desktops take the icon from the window, not from the theme. The PNG is
# used rather than the SVG because the pinned Qt build carries no SVG icon engine.
qt_add_resources(
  cullfinch_app
  "cullfinch_icons"
  PREFIX
  "/icons"
  BASE
  "${PROJECT_SOURCE_DIR}/packaging/linux/icons"
  FILES
  "${PROJECT_SOURCE_DIR}/packaging/linux/icons/org.fritze.cullfinch.png")

set_target_properties(
  cullfinch_app
  PROPERTIES AUTOMOC ON
             OUTPUT_NAME cullfinch
             WIN32_EXECUTABLE FALSE
             MACOSX_BUNDLE TRUE
             MACOSX_BUNDLE_BUNDLE_NAME cullfinch
             MACOSX_BUNDLE_GUI_IDENTIFIER org.fritze.cullfinch
             MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
             MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

# The desktop identity must match the installed .desktop basename exactly, so both come from one
# definition.
set(CULLFINCH_DESKTOP_ID "org.fritze.cullfinch")

target_compile_definitions(cullfinch_app PRIVATE CULLFINCH_VERSION="${PROJECT_VERSION}"
                                                 CULLFINCH_DESKTOP_ID="${CULLFINCH_DESKTOP_ID}")

target_link_libraries(cullfinch_app PRIVATE cullfinch::project_options
                                            cullfinch::cullfinch_composition Qt6::Widgets)

cullfinch_enable_static_analysis(cullfinch_app)
cullfinch_enable_coverage(cullfinch_app)

install(
  TARGETS cullfinch_app
  BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Qt's deployment-script generator handles the plugin and library layout for a non-QML application.
# Its results are validated against the pinned vcpkg build by the packaged-application smoke test.
#
# The helper is not complete on either platform, and the gaps differ.
#
# On Linux the generic deployment tool ships every plugin family a linked Qt module needs -- the
# SQLite driver arrives together with the sqlite3 library it links -- but only the platform plugin
# it considers the default, which is XCB. Native Wayland is the primary Linux backend, so its QPA
# plugin is requested explicitly below: without it the installed package silently runs through
# XWayland, which the design rules out. The wayland-shell-integration and
# wayland-graphics-integration plugins the tool does deploy are useless on their own.
#
# On macOS, macdeployqt deploys the Cocoa, image-format and style plugins unconditionally but
# decides about sqldrivers from the file names of the Qt libraries it deployed for the executable,
# where it expects libQt6Sql.6.dylib. vcpkg rewrites the install names of the libraries it builds to
# their fully versioned file names, so the executable links libQt6Sql.6.11.1.dylib, QtSql is never
# recognised as being in use, and the bundle got no sqldrivers directory at all:
#
# QSqlDatabase: can not load requested driver 'QSQLITE', available drivers:
#
# A hand-installed copy of the driver was not enough either. The driver links vcpkg's sqlite3
# library and the unversioned Qt names, none of which had been placed in Contents/Frameworks, so it
# was found but could not be loaded:
#
# QSqlDatabase: can not load requested driver 'QSQLITE', available drivers: QSQLITE
#
# So the driver is installed into the bundle before the deployment script runs and handed to
# macdeployqt as an additional binary. macdeployqt then deploys the driver's dependencies next to
# the executable's and rewrites its load paths the same way it does for the plugins it chose itself.
# Each plugin is added only where the helper leaves it out.
if(APPLE)
  if(NOT TARGET Qt6::QSQLiteDriverPlugin)
    message(
      FATAL_ERROR "cullfinch: the Qt SQLite driver plugin is missing from this Qt build. "
                  "cullfinch stores all of its metadata in SQLite and cannot start " "without it.")
  endif()
  set(CULLFINCH_SQLDRIVER_DIR "cullfinch.app/Contents/PlugIns/sqldrivers")
  install(FILES $<TARGET_FILE:Qt6::QSQLiteDriverPlugin> DESTINATION "${CULLFINCH_SQLDRIVER_DIR}")
  # The path is relative to the install prefix, which is the working directory macdeployqt runs in.
  set(CULLFINCH_DEPLOY_TOOL_OPTIONS
      DEPLOY_TOOL_OPTIONS
      "-executable=${CULLFINCH_SQLDRIVER_DIR}/$<TARGET_FILE_NAME:Qt6::QSQLiteDriverPlugin>")
endif()

qt_generate_deploy_app_script(
  TARGET
  cullfinch_app
  OUTPUT_SCRIPT
  cullfinch_deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR
  ${CULLFINCH_DEPLOY_TOOL_OPTIONS})
install(SCRIPT ${cullfinch_deploy_script})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(NOT TARGET Qt6::QWaylandIntegrationPlugin)
    message(
      FATAL_ERROR
        "cullfinch: the Qt Wayland platform plugin is missing from this Qt "
        "build. Native Wayland is the primary Linux backend; enable the "
        "'wayland' qtbase feature in vcpkg.json.")
  endif()
  install(FILES $<TARGET_FILE:Qt6::QWaylandIntegrationPlugin> DESTINATION Qt6/plugins/platforms)
endif()
