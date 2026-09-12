# Shared helpers for owned targets. Every owned library, executable and test goes through these so
# warnings, C++23, static analysis and coverage are applied consistently and never leak onto
# dependency targets.

include_guard(GLOBAL)

option(CULLFINCH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CULLFINCH_ENABLE_SANITIZERS "Build owned targets with ASan and UBSan" OFF)

add_library(cullfinch_project_options INTERFACE)
add_library(cullfinch::project_options ALIAS cullfinch_project_options)

target_compile_features(cullfinch_project_options INTERFACE cxx_std_23)

target_compile_options(
  cullfinch_project_options
  INTERFACE $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wcast-qual
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough>)

if(CULLFINCH_WARNINGS_AS_ERRORS)
  target_compile_options(cullfinch_project_options
                         INTERFACE $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Werror>)
endif()

# Qt headers are dependency headers: keep their diagnostics out of the way.
target_compile_definitions(
  cullfinch_project_options INTERFACE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
                                      QT_USE_QSTRINGBUILDER QT_DISABLE_DEPRECATED_UP_TO=0x060500)

if(CULLFINCH_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "cullfinch: sanitizers are supported with Clang and GCC only.")
  endif()
  target_compile_options(
    cullfinch_project_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer
                                        -fno-sanitize-recover=undefined)
  target_link_options(cullfinch_project_options INTERFACE -fsanitize=address,undefined)
endif()

# cullfinch_add_library(<name> SOURCES ... [HEADERS ...] [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
#
# Creates an owned static library plus a cullfinch:: alias. HEADERS are added to the source list so
# AUTOMOC sees Q_OBJECT declarations. The directory holding the module's include root is exposed
# publicly.
function(cullfinch_add_library name)
  cmake_parse_arguments(
    ARG
    ""
    ""
    "SOURCES;HEADERS;PUBLIC_DEPS;PRIVATE_DEPS"
    ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cullfinch_add_library(${name}): SOURCES is required.")
  endif()

  add_library(${name} STATIC ${ARG_SOURCES} ${ARG_HEADERS})
  add_library(cullfinch::${name} ALIAS ${name})

  set_target_properties(
    ${name}
    PROPERTIES AUTOMOC ON
               POSITION_INDEPENDENT_CODE ON
               EXPORT_COMPILE_COMMANDS ON)

  target_include_directories(${name} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${name} PUBLIC cullfinch::project_options ${ARG_PUBLIC_DEPS})
  if(ARG_PRIVATE_DEPS)
    target_link_libraries(${name} PRIVATE ${ARG_PRIVATE_DEPS})
  endif()

  cullfinch_enable_static_analysis(${name})
  cullfinch_enable_coverage(${name})
endfunction()

# cullfinch_add_test(<name> SOURCES ... [DEPS ...] [LABELS ...] [TIMEOUT <s>] [PLATFORM <qpa>])
#
# One CTest entry per independent test executable. Registration never inspects the display, so
# configuring on a headless machine always works; the Qt platform plugin is selected through the
# test environment instead.
function(cullfinch_add_test name)
  cmake_parse_arguments(
    ARG
    ""
    "TIMEOUT;PLATFORM"
    "SOURCES;DEPS;LABELS"
    ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "cullfinch_add_test(${name}): SOURCES is required.")
  endif()
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 120)
  endif()

  add_executable(${name} ${ARG_SOURCES})
  set_target_properties(${name} PROPERTIES AUTOMOC ON EXPORT_COMPILE_COMMANDS ON)
  target_link_libraries(${name} PRIVATE cullfinch::project_options Qt6::Test ${ARG_DEPS})

  cullfinch_enable_static_analysis(${name})
  cullfinch_enable_coverage(${name})

  add_test(NAME ${name} COMMAND ${name})

  set(_environment "CULLFINCH_TEST_MODE=1")
  if(ARG_PLATFORM)
    list(APPEND _environment "QT_QPA_PLATFORM=${ARG_PLATFORM}")
  endif()

  set_tests_properties(
    ${name}
    PROPERTIES LABELS
               "${ARG_LABELS}"
               TIMEOUT
               ${ARG_TIMEOUT}
               ENVIRONMENT
               "${_environment}"
               SKIP_RETURN_CODE
               77)
endfunction()
