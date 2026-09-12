cmake_minimum_required(VERSION 3.28)

# vcpkg acquisition must happen before project(): the toolchain, triplets and manifest features are
# only honoured while no toolchain has been loaded.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(BootstrapVcpkg)

project(
  cullfinch
  VERSION 0.1.0
  DESCRIPTION "Image collection culling utility"
  HOMEPAGE_URL "https://github.com/fritze-org/cullfinch"
  LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME MATCHES "Linux|Darwin")
  message(FATAL_ERROR "cullfinch targets Linux and macOS (R1); '${CMAKE_SYSTEM_NAME}' is not "
                      "supported.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      RelWithDebInfo
      CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(StaticAnalysis)
include(Coverage)
include(ProjectTargets)

find_package(
  Qt6 6.5 REQUIRED
  COMPONENTS Core
             Gui
             Widgets
             Concurrent
             Sql)

if(CULLFINCH_BUILD_TESTING)
  find_package(Qt6 6.5 REQUIRED COMPONENTS Test)
endif()

qt_standard_project_setup(REQUIRES 6.5)

message(STATUS "cullfinch: Qt ${Qt6_VERSION} from ${Qt6_DIR}")

add_subdirectory(src/domain)
add_subdirectory(src/application)
add_subdirectory(src/flows/versus)
add_subdirectory(src/flows/wall)
add_subdirectory(src/infrastructure)
add_subdirectory(src/ui)
add_subdirectory(src/views/versus)
add_subdirectory(src/views/wall)
add_subdirectory(src/app)

if(CULLFINCH_BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

add_subdirectory(packaging)

# ----------------------------------------------------------------- summary --
message(STATUS "cullfinch configuration:")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests           : ${CULLFINCH_BUILD_TESTING}")
message(STATUS "  clang-tidy      : ${CULLFINCH_ENABLE_CLANG_TIDY}")
message(STATUS "  coverage        : ${CULLFINCH_ENABLE_COVERAGE}")
message(STATUS "  sanitizers      : ${CULLFINCH_ENABLE_SANITIZERS}")
message(STATUS "  warnings->errors: ${CULLFINCH_WARNINGS_AS_ERRORS}")
