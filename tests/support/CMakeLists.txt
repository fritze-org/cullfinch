# Fixtures and test doubles shared by the suites. Not part of the shipped application and excluded
# from the coverage denominator.
cullfinch_add_library(
  cullfinch_testsupport
  SOURCES cullfinch/testsupport/AssetBuilder.cpp cullfinch/testsupport/FakeRepository.cpp
          cullfinch/testsupport/FakeTrashAdapter.cpp cullfinch/testsupport/TempCollection.cpp
  HEADERS cullfinch/testsupport/AssetBuilder.h cullfinch/testsupport/FakeRepository.h
          cullfinch/testsupport/FakeTrashAdapter.h cullfinch/testsupport/TempCollection.h
  PUBLIC_DEPS cullfinch::cullfinch_application Qt6::Gui)
