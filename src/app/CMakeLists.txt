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
qt_generate_deploy_app_script(
  TARGET
  cullfinch_app
  OUTPUT_SCRIPT
  cullfinch_deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${cullfinch_deploy_script})

# Qt's deployment helper installs only the platform plugin it considers the default, which on Linux
# is XCB. Native Wayland is the primary Linux backend, so its QPA plugin has to be requested
# explicitly: without it the installed package silently runs through XWayland, which the design
# rules out. The wayland-shell-integration and wayland-graphics-integration plugins the helper
# already deploys are useless on their own.
#
# The macOS bundle showed the same gap from the other direction: it received libQt6Sql and the
# Cocoa, image-format and style plugins, but no sqldrivers directory at all, so the application
# could not open its own database:
#
# QSqlDatabase: can not load requested driver 'QSQLITE', available drivers:
#
# Each plugin is added only where the helper leaves it out, so the ones it does deploy keep the load
# paths it rewrote for them.
if(APPLE)
  if(NOT TARGET Qt6::QSQLiteDriverPlugin)
    message(
      FATAL_ERROR "cullfinch: the Qt SQLite driver plugin is missing from this Qt build. "
                  "cullfinch stores all of its metadata in SQLite and cannot start " "without it.")
  endif()
  install(FILES $<TARGET_FILE:Qt6::QSQLiteDriverPlugin>
          DESTINATION "cullfinch.app/Contents/PlugIns/sqldrivers")

  # The helper deploys what the executable links, under the names the executable asks for, and
  # rewrites only the plugins it deploys itself. The driver above was linked inside the Qt build,
  # before vcpkg rewrote the install names, so it asks for libQt6Sql by its soname
  # (libQt6Sql.6.dylib) where the bundle holds only the executable's spelling
  # (libQt6Sql.6.11.1.dylib), and for vcpkg's sqlite3, which nothing else in the bundle needs. The
  # packaged smoke test showed the result: "can not load requested driver 'QSQLITE', available
  # drivers: QSQLITE". Both travel by hand under the names the driver asks for; the rpath the helper
  # gives the executable makes Contents/Frameworks visible to every plugin it loads.
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  foreach(dependency IN ITEMS unofficial::sqlite3::sqlite3 Qt6::Sql)
    install(
      FILES $<TARGET_FILE:${dependency}>
      DESTINATION "cullfinch.app/Contents/Frameworks"
      RENAME
        $<IF:$<BOOL:$<TARGET_SONAME_FILE_NAME:${dependency}>>,$<TARGET_SONAME_FILE_NAME:${dependency}>,$<TARGET_FILE_NAME:${dependency}>>
    )
  endforeach()
endif()

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
