# Coverage instrumentation and reporting (R15).
#
# GCC/gcov is the only implemented backend. Requesting coverage with another compiler fails
# configuration rather than producing an empty report. Only owned targets are instrumented; vcpkg
# and Qt dependencies never are.

include_guard(GLOBAL)

option(CULLFINCH_ENABLE_COVERAGE "Instrument owned targets for gcov coverage" OFF)

if(NOT CULLFINCH_ENABLE_COVERAGE)
  function(cullfinch_enable_coverage target)
    # Coverage disabled: nothing to do.
  endfunction()
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(
    FATAL_ERROR "cullfinch: CULLFINCH_ENABLE_COVERAGE=ON is implemented for GCC only "
                "(compiler is '${CMAKE_CXX_COMPILER_ID}'). Use the 'coverage' preset with GCC.")
endif()

# gcov must come from the same GCC version as the compiler, otherwise the .gcno/.gcda format will
# not match.
if(NOT CULLFINCH_GCOV_EXECUTABLE)
  get_filename_component(_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  string(REGEX MATCH "[0-9]+(\\.[0-9]+)*$" _gcc_suffix "${CMAKE_CXX_COMPILER}")
  find_program(
    CULLFINCH_GCOV_EXECUTABLE
    NAMES "gcov-${CMAKE_CXX_COMPILER_VERSION}" "gcov-${_gcc_suffix}" gcov
    HINTS "${_cxx_dir}"
    DOC "gcov matching the coverage compiler")
endif()

if(NOT CULLFINCH_GCOV_EXECUTABLE)
  message(FATAL_ERROR "cullfinch: coverage is enabled but no gcov executable was found.")
endif()

execute_process(
  COMMAND "${CULLFINCH_GCOV_EXECUTABLE}" --version
  OUTPUT_VARIABLE _gcov_version_output
  RESULT_VARIABLE _gcov_version_result
  TIMEOUT 120
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _gcov_version_result EQUAL 0)
  message(FATAL_ERROR "cullfinch: '${CULLFINCH_GCOV_EXECUTABLE} --version' failed.")
endif()

string(REGEX MATCH "([0-9]+)\\.[0-9]+\\.[0-9]+" _gcov_version "${_gcov_version_output}")
string(REGEX MATCH "^([0-9]+)" _gcc_major "${CMAKE_CXX_COMPILER_VERSION}")
if(NOT _gcov_version MATCHES "^${_gcc_major}\\.")
  message(
    FATAL_ERROR "cullfinch: gcov ('${_gcov_version}') does not match the compiler major version "
                "('${CMAKE_CXX_COMPILER_VERSION}'). Set CULLFINCH_GCOV_EXECUTABLE explicitly.")
endif()

message(STATUS "cullfinch: coverage enabled, gcov ${_gcov_version} "
               "(${CULLFINCH_GCOV_EXECUTABLE})")

set(CULLFINCH_COVERAGE_DIR
    "${PROJECT_BINARY_DIR}/coverage"
    CACHE PATH "coverage report directory")

# Instrument one owned target. Applied to libraries, the application and the test executables, so
# owned inline and header code exercised only by tests is measured too. -fprofile-abs-path keeps the
# .gcno paths resolvable from the report directory.
function(cullfinch_enable_coverage target)
  target_compile_options(
    ${target}
    PRIVATE --coverage
            -O0
            -g
            -fprofile-abs-path
            -fprofile-update=atomic)
  target_link_options(${target} PRIVATE --coverage)
endfunction()

# gcovr resolved through the pinned uv tooling group, so the report generator version is the same
# locally and in CI.
find_program(
  CULLFINCH_UV_EXECUTABLE
  NAMES uv
  DOC "uv, for the pinned tooling environment")
if(CULLFINCH_UV_EXECUTABLE)
  set(_gcovr_command "${CULLFINCH_UV_EXECUTABLE}" run --frozen --group tooling -- gcovr)
else()
  find_program(CULLFINCH_GCOVR_EXECUTABLE NAMES gcovr)
  if(NOT CULLFINCH_GCOVR_EXECUTABLE)
    message(FATAL_ERROR "cullfinch: neither uv nor gcovr was found; cannot generate coverage "
                        "reports. Run 'uv sync --locked --group tooling'.")
  endif()
  set(_gcovr_command "${CULLFINCH_GCOVR_EXECUTABLE}")
endif()

# Report generation only. It never reruns tests and never merges stale counters.
add_custom_target(
  coverage-report
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CULLFINCH_COVERAGE_DIR}/html"
  COMMAND
    ${_gcovr_command} --config "${PROJECT_SOURCE_DIR}/gcovr.cfg" --root "${PROJECT_SOURCE_DIR}"
    --gcov-executable "${CULLFINCH_GCOV_EXECUTABLE}" --cobertura
    "${CULLFINCH_COVERAGE_DIR}/coverage.xml" --cobertura-pretty --html-details
    "${CULLFINCH_COVERAGE_DIR}/html/index.html" --txt-summary "${PROJECT_BINARY_DIR}"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  COMMENT "Generating Cobertura XML and HTML coverage reports"
  VERBATIM)

# Remove counters only, inside the configured coverage build directory.
add_custom_target(
  coverage-reset
  COMMAND "${CMAKE_COMMAND}" -DCULLFINCH_COVERAGE_BINARY_DIR=${PROJECT_BINARY_DIR} -P
          "${PROJECT_SOURCE_DIR}/cmake/ResetCoverageCounters.cmake"
  COMMENT "Removing .gcda counters from the coverage build tree"
  VERBATIM)
