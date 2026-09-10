# Session lifecycle, validation, disposition changes and operation planning. It owns no concrete
# flow layout and decodes no image.
cullfinch_add_library(
  cullfinch_application
  SOURCES cullfinch/application/CollectionController.cpp
          cullfinch/application/DispositionController.cpp
          cullfinch/application/FlowRegistry.cpp
          cullfinch/application/OperationController.cpp
          cullfinch/application/SessionController.cpp
          cullfinch/application/Services.cpp
  HEADERS cullfinch/application/CollectionController.h
          cullfinch/application/DispositionController.h
          cullfinch/application/FlowRegistry.h
          cullfinch/application/ImageService.h
          cullfinch/application/OperationController.h
          cullfinch/application/SessionController.h
          cullfinch/application/Services.h
  PUBLIC_DEPS cullfinch::cullfinch_domain Qt6::Core Qt6::Gui)
