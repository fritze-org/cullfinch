# clang-tidy integration driven by CMake (R14).
#
# The CXX_CLANG_TIDY target property is set on owned targets only, through
# cullfinch_enable_static_analysis(). Imported and vcpkg dependency targets are never analysed.
# Under Ninja, CMake invokes clang-tidy with the real compilation command, so include paths and
# definitions are always correct.

include_guard(GLOBAL)

option(CULLFINCH_ENABLE_CLANG_TIDY "Run clang-tidy on owned targets during the build" OFF)
set(CULLFINCH_CLANG_TIDY_MIN_VERSION
    "19.0"
    CACHE STRING "Minimum supported clang-tidy version")

if(NOT CULLFINCH_ENABLE_CLANG_TIDY)
  function(cullfinch_enable_static_analysis target)
    # Analysis disabled: nothing to do.
  endfunction()
  return()
endif()

# Resolve a pinned, supported clang-tidy. Fail configuration when analysis is requested and the
# executable is missing or too old: an unanalysed build must never masquerade as a passing analysis
# run.
find_program(
  CULLFINCH_CLANG_TIDY_EXECUTABLE
  NAMES clang-tidy-23
        clang-tidy-22
        clang-tidy-21
        clang-tidy-20
        clang-tidy-19
        clang-tidy
  DOC "clang-tidy executable used for static analysis")

if(NOT CULLFINCH_CLANG_TIDY_EXECUTABLE)
  message(FATAL_ERROR "cullfinch: CULLFINCH_ENABLE_CLANG_TIDY=ON but no clang-tidy was found. "
                      "Install the pinned LLVM tools or set CULLFINCH_CLANG_TIDY_EXECUTABLE.")
endif()

execute_process(
  COMMAND "${CULLFINCH_CLANG_TIDY_EXECUTABLE}" --version
  OUTPUT_VARIABLE _tidy_version_output
  ERROR_VARIABLE _tidy_version_error
  RESULT_VARIABLE _tidy_version_result
  TIMEOUT 120
  OUTPUT_STRIP_TRAILING_WHITESPACE)

if(NOT _tidy_version_result EQUAL 0)
  message(FATAL_ERROR "cullfinch: '${CULLFINCH_CLANG_TIDY_EXECUTABLE} --version' failed "
                      "(exit ${_tidy_version_result}): ${_tidy_version_error}")
endif()

if(NOT _tidy_version_output MATCHES "version ([0-9]+\\.[0-9]+\\.[0-9]+)")
  message(FATAL_ERROR "cullfinch: cannot parse the clang-tidy version from:\n"
                      "${_tidy_version_output}")
endif()
set(CULLFINCH_CLANG_TIDY_VERSION "${CMAKE_MATCH_1}")

if(CULLFINCH_CLANG_TIDY_VERSION VERSION_LESS CULLFINCH_CLANG_TIDY_MIN_VERSION)
  message(FATAL_ERROR "cullfinch: clang-tidy ${CULLFINCH_CLANG_TIDY_VERSION} is older than the "
                      "supported minimum ${CULLFINCH_CLANG_TIDY_MIN_VERSION}.")
endif()

message(STATUS "cullfinch: clang-tidy ${CULLFINCH_CLANG_TIDY_VERSION} "
               "(${CULLFINCH_CLANG_TIDY_EXECUTABLE})")

# Record the expanded enabled-check list so LLVM upgrades show up as a reviewed diff rather than a
# silent change in diagnostics.
execute_process(
  COMMAND "${CULLFINCH_CLANG_TIDY_EXECUTABLE}" "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
          --list-checks
  OUTPUT_FILE "${PROJECT_BINARY_DIR}/clang-tidy-enabled-checks.txt"
  ERROR_VARIABLE _tidy_list_error
  RESULT_VARIABLE _tidy_list_result
  TIMEOUT 120)
if(NOT _tidy_list_result EQUAL 0)
  message(FATAL_ERROR "cullfinch: could not validate .clang-tidy with the pinned executable: "
                      "${_tidy_list_error}")
endif()

set(CULLFINCH_CLANG_TIDY_COMMAND
    "${CULLFINCH_CLANG_TIDY_EXECUTABLE}" "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    "--warnings-as-errors=*" "--quiet")

# Apply analysis to one owned target.
function(cullfinch_enable_static_analysis target)
  set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY ${CULLFINCH_CLANG_TIDY_COMMAND})
endfunction()
