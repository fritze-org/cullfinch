# The versus bracket: state and transitions only. It knows nothing about paths, SQL or the other
# comparison modes.
cullfinch_add_library(
  cullfinch_flow_versus
  SOURCES cullfinch/flows/versus/VersusFlow.cpp
  HEADERS cullfinch/flows/versus/VersusFlow.h
  PUBLIC_DEPS cullfinch::cullfinch_domain)
