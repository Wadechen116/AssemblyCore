# Qt Widgets part of AssemblyCoreIDE. Everything that needs Qt is here; the services below it do not.
include(${CMAKE_CURRENT_LIST_DIR}/../sources.cmake)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test LinguistTools)

set(CMAKE_AUTORCC ON)
add_library(acide_ui STATIC ${ACIDE_UI_SOURCES})
target_include_directories(acide_ui PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(acide_ui PUBLIC acide_services Qt6::Widgets)
# the translations (compiled .qm files are part of the repository, see translations/README.md)
target_sources(acide_ui PRIVATE translations/translations.qrc)       # AUTORCC embeds it; i18n.cpp initialises it (static library)
set(ACIDE_TS ${CMAKE_CURRENT_LIST_DIR}/translations/acide_zh_TW.ts)
add_custom_target(acide_update_translations
  COMMAND $<TARGET_FILE:Qt6::lupdate> ${CMAKE_CURRENT_LIST_DIR} -ts ${ACIDE_TS} -source-language en_US -target-language zh_TW -locations none -no-obsolete
  COMMAND $<TARGET_FILE:Qt6::lrelease> ${ACIDE_TS} -qm ${CMAKE_CURRENT_LIST_DIR}/translations/acide_zh_TW.qm
  COMMENT "Updating the translations (lupdate, lrelease)" VERBATIM)
if(MSVC)
  # Qt 6.5 needs stdext::make_checked_array_iterator, which the Visual Studio 2026 STL dropped (see the header)
  target_compile_options(acide_ui PUBLIC /FI${CMAKE_CURRENT_LIST_DIR}/msvc_stdext_compat.h)
  # headers included with <...> (Qt's) are somebody else's code: do not report warnings inside them
  target_compile_options(acide_ui PUBLIC /external:anglebrackets /external:W0)
endif()

set(CMAKE_AUTORCC ON)
if(WIN32)
  add_executable(AssemblyCoreIDE WIN32 main.cpp)
else()
  add_executable(AssemblyCoreIDE main.cpp)
endif()
target_link_libraries(AssemblyCoreIDE PRIVATE acide_ui)

add_executable(acide_ui_tests ../tests/ui_tests.cpp)
target_link_libraries(acide_ui_tests PRIVATE acide_ui Qt6::Test)
target_compile_definitions(acide_ui_tests PRIVATE ACIDE_TRANSLATIONS_DIR="${CMAKE_CURRENT_LIST_DIR}/translations")
if(MSVC)
  # C4701 inside Qt Test's template qtestmouse.h (instantiated from our tests; not our code)
  target_compile_options(acide_ui_tests PRIVATE /wd4701)
endif()
add_test(NAME acide_ui_tests COMMAND acide_ui_tests)
set_tests_properties(acide_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
