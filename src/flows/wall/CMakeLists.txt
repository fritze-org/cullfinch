# The image wall: candidate-set state, transitions and the layout policy. Pure geometry and pure
# reducers; no paths, SQL or widgets.
cullfinch_add_library(
  cullfinch_flow_wall
  SOURCES cullfinch/flows/wall/WallFlow.cpp cullfinch/flows/wall/WallLayout.cpp
  HEADERS cullfinch/flows/wall/WallFlow.h cullfinch/flows/wall/WallLayout.h
  PUBLIC_DEPS cullfinch::cullfinch_domain)
