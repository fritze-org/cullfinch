# The browser, the shared comparison shell, image widgets, the preview decoding behind them and
# controller adapters. It never mutates an image file directly.
cullfinch_add_library(
  cullfinch_ui
  SOURCES cullfinch/ui/AssetListModel.cpp
          cullfinch/ui/AssetPresentation.cpp
          cullfinch/ui/BrowserWindow.cpp
          cullfinch/ui/ComparisonShell.cpp
          cullfinch/ui/FlowView.cpp
          cullfinch/ui/ImageCanvas.cpp
          cullfinch/ui/PreviewLoader.cpp
          cullfinch/ui/RecoveryDialog.cpp
          cullfinch/ui/ReviewDialog.cpp
  HEADERS cullfinch/ui/AssetListModel.h
          cullfinch/ui/AssetPresentation.h
          cullfinch/ui/BrowserWindow.h
          cullfinch/ui/ComparisonShell.h
          cullfinch/ui/FlowView.h
          cullfinch/ui/ImageCanvas.h
          cullfinch/ui/PreviewLoader.h
          cullfinch/ui/RecoveryDialog.h
          cullfinch/ui/ReviewDialog.h
  PUBLIC_DEPS cullfinch::cullfinch_application Qt6::Widgets)
