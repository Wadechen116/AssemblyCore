# qmake build of the service-layer tests (no Qt needed at run time, qmake is only the build driver).
TEMPLATE = app
TARGET = acide_services_tests
CONFIG += console c++17
CONFIG -= app_bundle qt
include(sources.pri)
include(../core/core.pri)
SOURCES += $$SERVICE_SOURCES $$CORE_SOURCES $$IDE_DIR/tests/services_tests.cpp
unix:!macx: LIBS += -lpthread
