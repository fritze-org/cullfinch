# Wall presentation and its validated actions. No persistence, no deletion.
cullfinch_add_library(
  cullfinch_view_wall
  SOURCES cullfinch/views/wall/WallView.cpp
  HEADERS cullfinch/views/wall/WallView.h
  PUBLIC_DEPS cullfinch::cullfinch_ui cullfinch::cullfinch_flow_wall)
