# Desktop integration and package metadata.
#
# Packaging itself (AppImage, disk images) is driven from the release workflow around the CMake
# install tree; this directory only installs the platform metadata that has to travel with the
# application.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES linux/org.fritze.cullfinch.desktop
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
  install(FILES linux/org.fritze.cullfinch.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
endif()
