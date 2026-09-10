# Acquire and validate a pinned vcpkg checkout, then hand control to vcpkg.
#
# This module MUST be included before the first project() call: every vcpkg-affecting variable has
# to be set while CMake has not yet loaded a toolchain. It implements checkout acquisition and
# validation only; vcpkg bootstraps its own executable and installs the manifest dependencies.
#
# Knobs: CULLFINCH_VCPKG_ROOT   Use a pre-provisioned, developer-owned checkout. It is validated but
# never reset or mutated. CULLFINCH_OFFLINE      Forbid all network activity during acquisition.
# CULLFINCH_VCPKG_CACHE  Managed checkout parent directory.

include_guard(GLOBAL)

if(DEFINED CULLFINCH_BOOTSTRAP_VCPKG_DONE)
  return()
endif()

if(DEFINED PROJECT_NAME)
  message(FATAL_ERROR "BootstrapVcpkg.cmake must be included before project().")
endif()

set(_cullfinch_source_dir "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_path(SET _cullfinch_source_dir NORMALIZE "${_cullfinch_source_dir}")

option(CULLFINCH_OFFLINE "Forbid network access while acquiring vcpkg and its dependencies" OFF)

# ---------------------------------------------------------------------------
# Helper: run an external process with a checked exit status, captured diagnostics and a useful
# timeout. A failure is never silently ignored.
# ---------------------------------------------------------------------------
function(_cullfinch_run)
  cmake_parse_arguments(
    ARG
    ""
    "WORKING_DIRECTORY;TIMEOUT;RESULT;OUTPUT;WHAT"
    "COMMAND"
    ${ARGN})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 900)
  endif()
  if(NOT ARG_WORKING_DIRECTORY)
    set(ARG_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
  endif()

  execute_process(
    COMMAND ${ARG_COMMAND}
    WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}"
    TIMEOUT ${ARG_TIMEOUT}
    RESULT_VARIABLE _code
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)

  if(ARG_RESULT)
    set(${ARG_RESULT}
        "${_code}"
        PARENT_SCOPE)
  endif()
  if(ARG_OUTPUT)
    set(${ARG_OUTPUT}
        "${_out}"
        PARENT_SCOPE)
  endif()
  if(NOT ARG_RESULT AND NOT _code EQUAL 0)
    message(FATAL_ERROR "cullfinch: ${ARG_WHAT} failed (exit ${_code}).\n"
                        "  command: ${ARG_COMMAND}\n" "  stdout : ${_out}\n" "  stderr : ${_err}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Step 1: Read the pinned commit from the manifest. It serves as both the registry baseline and the
# managed checkout revision; using one value avoids drift.
# ---------------------------------------------------------------------------
set(_manifest "${_cullfinch_source_dir}/vcpkg.json")
if(NOT EXISTS "${_manifest}")
  message(FATAL_ERROR "cullfinch: vcpkg.json not found at ${_manifest}.")
endif()

file(READ "${_manifest}" _manifest_text)
string(
  JSON
  _cullfinch_baseline
  ERROR_VARIABLE
  _json_error
  GET
  "${_manifest_text}"
  "builtin-baseline")
# CMake's regex dialect has no bounded repetition, so the length is checked separately rather than
# with a `{40}` quantifier that would silently never match.
string(LENGTH "${_cullfinch_baseline}" _baseline_length)
if(NOT _baseline_length EQUAL 40 OR NOT _cullfinch_baseline MATCHES "^[0-9a-f]+$")
  message(
    FATAL_ERROR
      "cullfinch: vcpkg.json needs a 40-character lowercase hexadecimal 'builtin-baseline' "
      "commit (got '${_cullfinch_baseline}', length ${_baseline_length}, JSON error "
      "'${_json_error}').")
endif()

# ---------------------------------------------------------------------------
# Step 2: Manifest features. Selected here because VCPKG_MANIFEST_FEATURES must be set before
# project() loads the vcpkg toolchain.
# ---------------------------------------------------------------------------
option(CULLFINCH_BUILD_TESTING "Build the cullfinch test suites" ON)

set(_features "")
if(CULLFINCH_BUILD_TESTING)
  list(APPEND _features "tests")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND _features "linux-desktop")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND _features "macos-desktop")
endif()
set(VCPKG_MANIFEST_FEATURES
    ${_features}
    CACHE STRING "vcpkg manifest features" FORCE)

# ---------------------------------------------------------------------------
# Step 3: Triplets. Repository-owned dynamic-linkage triplets live in cmake/triplets. The host
# triplet is chosen deliberately so Qt's build tools are usable.
# ---------------------------------------------------------------------------
set(VCPKG_OVERLAY_TRIPLETS
    "${_cullfinch_source_dir}/cmake/triplets"
    CACHE STRING "cullfinch overlay triplets" FORCE)

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
      set(_triplet "x64-osx-cullfinch")
    elseif(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
      set(_triplet "arm64-osx-cullfinch")
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
      set(_triplet "arm64-osx-cullfinch")
    else()
      set(_triplet "x64-osx-cullfinch")
    endif()
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(_triplet "x64-linux-cullfinch")
  else()
    message(FATAL_ERROR "cullfinch supports Linux and macOS only "
                        "(host is '${CMAKE_HOST_SYSTEM_NAME}').")
  endif()
  set(VCPKG_TARGET_TRIPLET
      "${_triplet}"
      CACHE STRING "vcpkg target triplet" FORCE)
endif()

if(NOT DEFINED VCPKG_HOST_TRIPLET)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
      set(VCPKG_HOST_TRIPLET
          "arm64-osx-cullfinch"
          CACHE STRING "vcpkg host triplet" FORCE)
    else()
      set(VCPKG_HOST_TRIPLET
          "x64-osx-cullfinch"
          CACHE STRING "vcpkg host triplet" FORCE)
    endif()
  else()
    set(VCPKG_HOST_TRIPLET
        "x64-linux-cullfinch"
        CACHE STRING "vcpkg host triplet" FORCE)
  endif()
endif()

# ---------------------------------------------------------------------------
# Step 4: Locate the checkout: developer-owned, or the managed one keyed by commit.
# ---------------------------------------------------------------------------
find_package(Git QUIET)

# Verify that <dir> is a Git repository checked out at exactly <commit>.
function(_cullfinch_verify_checkout dir commit out_var)
  set(${out_var}
      FALSE
      PARENT_SCOPE)
  if(NOT EXISTS "${dir}/.git" OR NOT EXISTS "${dir}/bootstrap-vcpkg.sh")
    return()
  endif()
  if(NOT GIT_EXECUTABLE)
    return()
  endif()
  _cullfinch_run(
    COMMAND
    "${GIT_EXECUTABLE}"
    rev-parse
    HEAD
    WORKING_DIRECTORY
    "${dir}"
    TIMEOUT
    120
    RESULT
    _code
    OUTPUT
    _head
    WHAT
    "reading the vcpkg checkout revision")
  if(_code EQUAL 0 AND _head STREQUAL "${commit}")
    set(${out_var}
        TRUE
        PARENT_SCOPE)
  endif()
endfunction()

if(DEFINED CULLFINCH_VCPKG_ROOT OR DEFINED ENV{CULLFINCH_VCPKG_ROOT})
  # Pre-provisioned installation. Validate, never mutate.
  if(NOT DEFINED CULLFINCH_VCPKG_ROOT)
    set(CULLFINCH_VCPKG_ROOT "$ENV{CULLFINCH_VCPKG_ROOT}")
  endif()
  cmake_path(SET _vcpkg_root NORMALIZE "${CULLFINCH_VCPKG_ROOT}")

  if(NOT EXISTS "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
    message(FATAL_ERROR "cullfinch: CULLFINCH_VCPKG_ROOT='${_vcpkg_root}' is not a vcpkg checkout.")
  endif()

  _cullfinch_verify_checkout("${_vcpkg_root}" "${_cullfinch_baseline}" _root_matches)
  if(NOT _root_matches)
    message(
      WARNING "cullfinch: CULLFINCH_VCPKG_ROOT='${_vcpkg_root}' is not at the pinned baseline "
              "${_cullfinch_baseline}. The developer-owned checkout is used as-is and is never "
              "reset; resolved dependency versions may differ from CI.")
  endif()
  message(STATUS "cullfinch: using developer-provided vcpkg at ${_vcpkg_root}")
else()
  if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "cullfinch: Git is required to acquire the pinned vcpkg checkout. "
                        "Install Git or set CULLFINCH_VCPKG_ROOT to a provisioned checkout.")
  endif()

  if(NOT DEFINED CULLFINCH_VCPKG_CACHE)
    if(DEFINED ENV{CULLFINCH_VCPKG_CACHE})
      set(CULLFINCH_VCPKG_CACHE "$ENV{CULLFINCH_VCPKG_CACHE}")
    else()
      set(CULLFINCH_VCPKG_CACHE "${_cullfinch_source_dir}/.vcpkg")
    endif()
  endif()
  cmake_path(SET CULLFINCH_VCPKG_CACHE NORMALIZE "${CULLFINCH_VCPKG_CACHE}")

  string(SUBSTRING "${_cullfinch_baseline}" 0 12 _short_baseline)
  set(_vcpkg_root "${CULLFINCH_VCPKG_CACHE}/vcpkg-${_short_baseline}")
  file(MAKE_DIRECTORY "${CULLFINCH_VCPKG_CACHE}")

  # A lock around creating or inspecting a possibly incomplete checkout, so concurrent configures
  # cannot observe or publish a partial tree.
  file(
    LOCK "${CULLFINCH_VCPKG_CACHE}/acquire.lock"
    GUARD PROCESS
    TIMEOUT 1800
    RESULT_VARIABLE _lock_result)
  if(NOT _lock_result EQUAL 0)
    message(FATAL_ERROR "cullfinch: could not lock the vcpkg cache directory: ${_lock_result}")
  endif()

  _cullfinch_verify_checkout("${_vcpkg_root}" "${_cullfinch_baseline}" _cached_ok)

  if(_cached_ok)
    message(STATUS "cullfinch: reusing pinned vcpkg checkout ${_vcpkg_root}")
  else()
    if(EXISTS "${_vcpkg_root}")
      # Present but not valid: an interrupted acquisition. Never mistake it for a usable cache.
      message(STATUS "cullfinch: discarding an incomplete vcpkg checkout at ${_vcpkg_root}")
      file(REMOVE_RECURSE "${_vcpkg_root}")
    endif()

    if(CULLFINCH_OFFLINE)
      message(
        FATAL_ERROR
          "cullfinch: CULLFINCH_OFFLINE=ON but no valid vcpkg checkout for baseline "
          "${_cullfinch_baseline} exists at ${_vcpkg_root}.\n"
          "  Provision it beforehand, or point CULLFINCH_VCPKG_ROOT at an existing checkout, "
          "and populate the vcpkg downloads and binary caches.")
    endif()

    set(_staging "${CULLFINCH_VCPKG_CACHE}/incoming-${_short_baseline}")
    file(REMOVE_RECURSE "${_staging}")

    message(STATUS "cullfinch: fetching vcpkg ${_cullfinch_baseline} (this happens once per pin)")
    # A real Git repository with full history, so versioned port resolution can reach the git-tree
    # objects it needs. Blobs are fetched lazily.
    _cullfinch_run(
      COMMAND
      "${GIT_EXECUTABLE}"
      clone
      --filter=blob:none
      --no-checkout
      "https://github.com/microsoft/vcpkg.git"
      "${_staging}"
      WORKING_DIRECTORY
      "${CULLFINCH_VCPKG_CACHE}"
      TIMEOUT
      1800
      WHAT
      "cloning the vcpkg repository")

    _cullfinch_run(
      COMMAND
      "${GIT_EXECUTABLE}"
      checkout
      --detach
      "${_cullfinch_baseline}"
      WORKING_DIRECTORY
      "${_staging}"
      TIMEOUT
      900
      WHAT
      "checking out the pinned vcpkg revision ${_cullfinch_baseline}")

    # Verify the resolved commit before publishing the checkout.
    _cullfinch_verify_checkout("${_staging}" "${_cullfinch_baseline}" _staged_ok)
    if(NOT _staged_ok)
      file(REMOVE_RECURSE "${_staging}")
      message(FATAL_ERROR "cullfinch: the fetched vcpkg checkout is not at ${_cullfinch_baseline}.")
    endif()

    file(RENAME "${_staging}" "${_vcpkg_root}")
    message(STATUS "cullfinch: published vcpkg checkout at ${_vcpkg_root}")
  endif()

  file(LOCK "${CULLFINCH_VCPKG_CACHE}/acquire.lock" RELEASE)
endif()

# ---------------------------------------------------------------------------
# Step 5: Hand over to vcpkg. Everything below must precede project().
# ---------------------------------------------------------------------------
set(_vcpkg_toolchain "${_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")

if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "${_vcpkg_toolchain}")
  cmake_path(SET _existing NORMALIZE "${CMAKE_TOOLCHAIN_FILE}")
  if(NOT _existing STREQUAL _vcpkg_toolchain)
    message(
      FATAL_ERROR
        "cullfinch: an external CMAKE_TOOLCHAIN_FILE ('${CMAKE_TOOLCHAIN_FILE}') conflicts "
        "with the managed vcpkg toolchain. Supply cross-compilation settings through "
        "VCPKG_CHAINLOAD_TOOLCHAIN_FILE instead of replacing the toolchain.")
  endif()
endif()

set(CMAKE_TOOLCHAIN_FILE
    "${_vcpkg_toolchain}"
    CACHE FILEPATH "vcpkg toolchain" FORCE)
set(VCPKG_MANIFEST_MODE
    ON
    CACHE BOOL "" FORCE)
set(VCPKG_MANIFEST_INSTALL
    ON
    CACHE BOOL "" FORCE)
set(VCPKG_INSTALL_OPTIONS
    "--x-abi-tools-use-exact-versions"
    CACHE STRING "" FORCE)

if(CULLFINCH_OFFLINE)
  # A real network-disabled acquisition policy: vcpkg must find every source and binary artefact in
  # its caches or fail with a precise report.
  set(ENV{VCPKG_DISABLE_METRICS} "1")
  set(ENV{X_VCPKG_ASSET_SOURCES} "x-block-origin")
  list(APPEND VCPKG_INSTALL_OPTIONS "--only-downloads-from-cache")
  set(VCPKG_INSTALL_OPTIONS
      "${VCPKG_INSTALL_OPTIONS}"
      CACHE STRING "" FORCE)
endif()

set(CULLFINCH_VCPKG_BASELINE
    "${_cullfinch_baseline}"
    CACHE INTERNAL "pinned vcpkg baseline")
set(CULLFINCH_VCPKG_ROOT_RESOLVED
    "${_vcpkg_root}"
    CACHE INTERNAL "resolved vcpkg checkout")
set(CULLFINCH_BOOTSTRAP_VCPKG_DONE
    TRUE
    CACHE INTERNAL "bootstrap completed")

message(STATUS "cullfinch: vcpkg baseline ${_cullfinch_baseline}")
message(STATUS "cullfinch: vcpkg triplet  ${VCPKG_TARGET_TRIPLET} (host ${VCPKG_HOST_TRIPLET})")
message(STATUS "cullfinch: vcpkg features ${VCPKG_MANIFEST_FEATURES}")
