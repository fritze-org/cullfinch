# The domain owns identities, asset metadata, selection snapshots, pairing policy and the flow
# contracts. It must never reach for widgets, SQL connections or physical file operations, and its
# tests run without a GUI application.
cullfinch_add_library(
  cullfinch_domain
  SOURCES cullfinch/domain/AssociationPolicy.cpp
          cullfinch/domain/FileMember.cpp
          cullfinch/domain/FlowContract.cpp
          cullfinch/domain/NaturalOrder.cpp
          cullfinch/domain/OperationPlan.cpp
          cullfinch/domain/PhotoAsset.cpp
          cullfinch/domain/SelectionSnapshot.cpp
  HEADERS cullfinch/domain/AssociationPolicy.h
          cullfinch/domain/FileMember.h
          cullfinch/domain/FlowContract.h
          cullfinch/domain/Ids.h
          cullfinch/domain/NaturalOrder.h
          cullfinch/domain/OperationPlan.h
          cullfinch/domain/PhotoAsset.h
          cullfinch/domain/SelectionSnapshot.h
  PUBLIC_DEPS Qt6::Core)
