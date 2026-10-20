# qmake build of the UI tests; run with QT_QPA_PLATFORM=offscreen
TEMPLATE = app
TARGET = acide_ui_tests
QT += widgets testlib
CONFIG += c++17 console
CONFIG -= app_bundle
include(sources.pri)
include(../core/core.pri)
SOURCES += $$SERVICE_SOURCES $$UI_SOURCES $$CORE_SOURCES $$IDE_DIR/tests/ui_tests.cpp
HEADERS += $$UI_MOC_HEADERS
DEFINES += ACIDE_TRANSLATIONS_DIR=\\\"$$IDE_DIR/ui/translations\\\"
RESOURCES += $$UI_RESOURCES
unix:!macx: LIBS += -lpthread
