# Fixtures and test doubles shared by the suites. Not part of the shipped application and excluded
# from the coverage denominator.
cullfinch_add_library(
  cullfinch_testsupport
  SOURCES cullfinch/testsupport/AssetBuilder.cpp
          cullfinch/testsupport/FakeImageService.cpp
          cullfinch/testsupport/FakeRepository.cpp
          cullfinch/testsupport/FakeTrashAdapter.cpp
          cullfinch/testsupport/TempCollection.cpp
  HEADERS cullfinch/testsupport/AssetBuilder.h
          cullfinch/testsupport/CrashPoints.h
          cullfinch/testsupport/FakeImageService.h
          cullfinch/testsupport/FakeRepository.h
          cullfinch/testsupport/FakeTrashAdapter.h
          cullfinch/testsupport/TempCollection.h
  PUBLIC_DEPS cullfinch::cullfinch_application Qt6::Gui)

# The child process the crash-injection suite kills. It is an executable rather than a test because
# dying is its whole purpose: it runs the production controller, executor and SQLite journal, and
# calls _exit() at the journal write CULLFINCH_CRASH_AT names. The suite links none of it and finds
# the binary by path, so the build order is declared where that test is registered.
add_executable(cullfinch_crash_helper crashhelper/main.cpp)
set_target_properties(cullfinch_crash_helper PROPERTIES AUTOMOC ON EXPORT_COMPILE_COMMANDS ON)
target_link_libraries(
  cullfinch_crash_helper PRIVATE cullfinch::project_options cullfinch::cullfinch_infrastructure
                                 cullfinch::cullfinch_testsupport)

cullfinch_enable_static_analysis(cullfinch_crash_helper)
# Coverage instrumentation is inherited from the libraries it links, so the link options have to
# match them; the helper writes no profile of its own, because _exit() never flushes one.
cullfinch_enable_coverage(cullfinch_crash_helper)
