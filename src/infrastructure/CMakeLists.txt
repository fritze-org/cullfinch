# Scan adapters, the image service, the repository, staging and the Trash adapter. It owns no user
# selection and no comparison policy.
cullfinch_add_library(
  cullfinch_infrastructure
  SOURCES cullfinch/infrastructure/AppLock.cpp
          cullfinch/infrastructure/DirectoryScanner.cpp
          cullfinch/infrastructure/Paths.cpp
          cullfinch/infrastructure/QtImageService.cpp
          cullfinch/infrastructure/SqliteRepository.cpp
          cullfinch/infrastructure/StagingExecutor.cpp
          cullfinch/infrastructure/TrashAdapter.cpp
  HEADERS cullfinch/infrastructure/AppLock.h
          cullfinch/infrastructure/DirectoryScanner.h
          cullfinch/infrastructure/Paths.h
          cullfinch/infrastructure/QtImageService.h
          cullfinch/infrastructure/SqliteRepository.h
          cullfinch/infrastructure/StagingExecutor.h
          cullfinch/infrastructure/TrashAdapter.h
  PUBLIC_DEPS cullfinch::cullfinch_application Qt6::Sql
  PRIVATE_DEPS Qt6::Concurrent Qt6::Gui)
