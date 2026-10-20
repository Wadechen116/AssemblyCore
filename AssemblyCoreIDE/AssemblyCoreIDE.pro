# qmake build of the IDE:  qmake AssemblyCoreIDE.pro && make        (CMake: see CMakeLists.txt)
TEMPLATE = app
TARGET = AssemblyCoreIDE
QT += widgets
CONFIG += c++17
include(sources.pri)
include(../core/core.pri)
SOURCES += $$SERVICE_SOURCES $$UI_SOURCES $$CORE_SOURCES $$IDE_DIR/ui/main.cpp
HEADERS += $$UI_MOC_HEADERS
RESOURCES += $$UI_RESOURCES
unix:!macx: LIBS += -lpthread
win32: CONFIG += windows
