# The standalone crop tool: every photo in one directory, one after another, each to crop or skip.
# It reuses the scanner's grouping and the image service, and owns no session, no marks, no database
# and no Trash. The only files it writes are new ones beside a photo -- NAME.crop.jpg and the Camera
# Raw sidecar NAME.xmp -- and it never changes an original. See docs/decisions/0012.
find_package(exiv2 CONFIG REQUIRED)

cullfinch_add_library(
  cullfinch_cropper
  SOURCES cullfinch/cropper/CropCanvas.cpp
          cullfinch/cropper/CropGeometry.cpp
          cullfinch/cropper/CropSequence.cpp
          cullfinch/cropper/CropWindow.cpp
          cullfinch/cropper/CropWriter.cpp
          cullfinch/cropper/Sidecar.cpp
  HEADERS cullfinch/cropper/CropCanvas.h
          cullfinch/cropper/CropGeometry.h
          cullfinch/cropper/CropSequence.h
          cullfinch/cropper/CropWindow.h
          cullfinch/cropper/CropWriter.h
          cullfinch/cropper/Sidecar.h
  PUBLIC_DEPS cullfinch::cullfinch_ui Qt6::Widgets
  PRIVATE_DEPS cullfinch::cullfinch_infrastructure Exiv2::exiv2lib)

qt_add_executable(cullfinch_crop_app main.cpp)

qt_add_resources(
  cullfinch_crop_app
  "cullfinch_crop_icons"
  PREFIX
  "/icons"
  BASE
  "${PROJECT_SOURCE_DIR}/packaging/linux/icons"
  FILES
  "${PROJECT_SOURCE_DIR}/packaging/linux/icons/org.fritze.cullfinch.png")

set_target_properties(
  cullfinch_crop_app
  PROPERTIES AUTOMOC ON
             OUTPUT_NAME cullfinch-crop
             WIN32_EXECUTABLE FALSE
             MACOSX_BUNDLE TRUE
             MACOSX_BUNDLE_BUNDLE_NAME cullfinch-crop
             MACOSX_BUNDLE_GUI_IDENTIFIER org.fritze.cullfinch.crop
             MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
             MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

# Matches the installed .desktop basename, as for the full application.
target_compile_definitions(
  cullfinch_crop_app PRIVATE CULLFINCH_VERSION="${PROJECT_VERSION}"
                             CULLFINCH_DESKTOP_ID="org.fritze.cullfinch.crop")

target_link_libraries(
  cullfinch_crop_app
  PRIVATE cullfinch::project_options
          cullfinch::cullfinch_cropper
          cullfinch::cullfinch_infrastructure
          cullfinch::cullfinch_desktop
          Qt6::Widgets)

cullfinch_enable_static_analysis(cullfinch_crop_app)
cullfinch_enable_coverage(cullfinch_crop_app)

install(
  TARGETS cullfinch_crop_app
  BUNDLE DESTINATION .
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The same deployment helper as the other executables. exiv2 is a shared library in the vcpkg build,
# and the deploy script carries it along with the Qt libraries.
qt_generate_deploy_app_script(
  TARGET
  cullfinch_crop_app
  OUTPUT_SCRIPT
  cullfinch_crop_deploy_script
  NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${cullfinch_crop_deploy_script})
