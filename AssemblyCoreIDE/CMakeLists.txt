# AssemblyCoreIDE: services (standard C++17, no Qt) and the Qt Widgets application.
# Included from the top-level CMakeLists.txt when ASMCORE_BUILD_IDE is ON. The file lists live in sources.cmake (CMake) and sources.pri
# (qmake); tests/check_build_files.py verifies that they agree.
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

add_library(acide_services STATIC ${ACIDE_SERVICE_SOURCES})
target_include_directories(acide_services PUBLIC ${CMAKE_CURRENT_LIST_DIR}/services)
target_link_libraries(acide_services PUBLIC asmcore)
target_compile_features(acide_services PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(acide_services PUBLIC Threads::Threads)

add_executable(acide_services_tests tests/services_tests.cpp)
target_link_libraries(acide_services_tests PRIVATE acide_services)
target_compile_definitions(acide_services_tests PRIVATE ACIDE_SAMPLE_DIR="${CMAKE_CURRENT_LIST_DIR}/../sample")
add_test(NAME acide_services_tests COMMAND acide_services_tests)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME acide_build_files COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tests/check_build_files.py)
  add_test(NAME acide_translations COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tests/check_translations.py)
endif()

find_package(Qt6 QUIET COMPONENTS Widgets Test)
if(Qt6_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_subdirectory(ui)
else()
  message(STATUS "Qt6 not found: AssemblyCoreIDE builds the services and their tests only")
endif()
