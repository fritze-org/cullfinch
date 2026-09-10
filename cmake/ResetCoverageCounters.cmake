# Remove gcov counters (.gcda) from the coverage build tree. Compilation notes (.gcno) are retained
# so unexecuted translation units still appear in the report denominator.
if(NOT DEFINED CULLFINCH_COVERAGE_BINARY_DIR)
  message(FATAL_ERROR "CULLFINCH_COVERAGE_BINARY_DIR must be set.")
endif()

file(GLOB_RECURSE _counters "${CULLFINCH_COVERAGE_BINARY_DIR}/*.gcda")
list(LENGTH _counters _count)
if(_count GREATER 0)
  file(REMOVE ${_counters})
endif()
message(STATUS "cullfinch: removed ${_count} .gcda counter files")
