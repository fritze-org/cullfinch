# Desktop integration and package metadata.
#
# Packaging itself (AppImage, disk images) is driven from the release workflow around the CMake
# install tree; this directory only installs the platform metadata that has to travel with the
# application.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES linux/org.fritze.cullfinch.desktop linux/org.fritze.cullfinch.wall.desktop
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
  install(FILES linux/org.fritze.cullfinch.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
  # Named after the desktop id so the compositor can find it from the app id alone: on Wayland there
  # is no window-class fallback, and setDesktopFileName() is the only link.
  install(FILES linux/icons/org.fritze.cullfinch.svg
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
  install(FILES linux/icons/org.fritze.cullfinch.png
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps)
endif()
