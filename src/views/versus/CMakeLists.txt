# Versus presentation and its validated actions. No persistence, no deletion.
cullfinch_add_library(
  cullfinch_view_versus
  SOURCES cullfinch/views/versus/VersusView.cpp
  HEADERS cullfinch/views/versus/VersusView.h
  PUBLIC_DEPS cullfinch::cullfinch_ui cullfinch::cullfinch_flow_versus)
