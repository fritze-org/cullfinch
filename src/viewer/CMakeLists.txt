# The standalone image wall: every photo in one directory on one wall, to look at. It reuses the
# comparison wall's surface, the scanner's grouping and the image service, and owns no session, no
# marks, no database and no file operation: nothing it does can change a file.
cullfinch_add_library(
  cullfinch_viewer
  SOURCES cullfinch/viewer/DirectoryPhotos.cpp cullfinch/viewer/WallViewerWindow.cpp
  HEADERS cullfinch/viewer/DirectoryPhotos.h cullfinch/viewer/WallViewerWindow.h
  PUBLIC_DEPS cullfinch::cullfinch_view_wall
  PRIVATE_DEPS cullfinch::cullfinch_infrastructure)

qt_add_executable(cullfinch_wall_app main.cpp)

qt_add_resources(
  cullfinch_wall_app
  "cullfinch_wall_icons"
  PREFIX
  "/icons"
  BASE
  "${PROJECT_SOURCE_DIR}/packaging/linux/icons"
  FILES
  "${PROJECT_SOURCE_DIR}/packaging/linux/icons/org.fritze.cullfinch.png")

set_target_properties(
  cullfinch_wall_app
  PROPERTIES AUTOMOC ON
             OUTPUT_NAME cullfinch-wall
             WIN32_EXECUTABLE FALSE
             MACOSX_BUNDLE TRUE
             MACOSX_BUNDLE_BUNDLE_NAME cullfinch-wall
             MACOSX_BUNDLE_GUI_IDENTIFIER org.fritze.cullfinch.wall
             MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
             MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

# Matches the installed .desktop basename, as for the full application.
target_compile_definitions(
  cullfinch_wall_app PRIVATE CULLFINCH_VERSION="${PROJECT_VERSION}"
                             CULLFINCH_DESKTOP_ID="org.fritze.cullfinch.wall")

target_link_libraries(
  cullfinch_wall_app
  PRIVATE cullfinch::project_options
          cullfinch::cullfinch_viewer
          cullfinch::cullfinch_infrastructure
          cullfinch::cullfinch_desktop
          Qt6::Widgets)

cullfinch_enable_static_analysis(cullfinch_wall_app)
cullfinch_enable_coverage(cullfinch_wall_app)

install(
  TARGETS cullfinch_wall_app
  BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The same deployment helper as the full application. On Linux both deploy into one shared tree, and
# the native Wayland platform plugin src/app installs there serves this executable too. The viewer
# never opens a database, so the macOS SQLite driver workaround in src/app is not needed here.
qt_generate_deploy_app_script(
  TARGET
  cullfinch_wall_app
  OUTPUT_SCRIPT
  cullfinch_wall_deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${cullfinch_wall_deploy_script})
