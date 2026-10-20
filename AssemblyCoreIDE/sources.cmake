# Source lists of the IDE. Keep in step with sources.pri.
set(ACIDE_SERVICE_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/services/fs.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/project.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/workspace.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/build.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/settings.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/editing.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/artifacts.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/sim_session.cpp
  ${CMAKE_CURRENT_LIST_DIR}/services/debugger.cpp
)

set(ACIDE_UI_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/ui/qt_dispatcher.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/theme.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/syntax_highlighter.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/code_editor.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/panels.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/debug_panels.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/templates.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/find_bar.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/main_window.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/i18n.cpp
  ${CMAKE_CURRENT_LIST_DIR}/ui/app_shell.cpp
)
