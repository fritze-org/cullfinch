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
