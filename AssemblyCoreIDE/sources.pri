# qmake counterpart of sources.cmake (tests/check_build_files.py keeps them equal).
IDE_DIR = $$PWD
INCLUDEPATH += $$IDE_DIR/services $$IDE_DIR/ui
# Qt 6.5 needs stdext::make_checked_array_iterator, which the Visual Studio 2026 STL dropped (ui/msvc_stdext_compat.h)
win32-msvc*: QMAKE_CXXFLAGS += /FI$$shell_quote($$IDE_DIR/ui/msvc_stdext_compat.h) /external:anglebrackets /external:W0

SERVICE_SOURCES = \
    $$IDE_DIR/services/fs.cpp \
    $$IDE_DIR/services/project.cpp \
    $$IDE_DIR/services/workspace.cpp \
    $$IDE_DIR/services/build.cpp \
    $$IDE_DIR/services/settings.cpp \
    $$IDE_DIR/services/editing.cpp \
    $$IDE_DIR/services/artifacts.cpp \
    $$IDE_DIR/services/sim_session.cpp \
    $$IDE_DIR/services/debugger.cpp \
    $$IDE_DIR/services/hardware_link.cpp \
    $$IDE_DIR/services/blocks/block_theme.cpp \
    $$IDE_DIR/services/blocks/block_shapes.cpp \
    $$IDE_DIR/services/blocks/block_layout.cpp \
    $$IDE_DIR/services/blocks/block_session.cpp \
    $$IDE_DIR/services/blocks/block_drop.cpp \
    $$IDE_DIR/services/blocks/block_tidy.cpp

UI_SOURCES = \
    $$IDE_DIR/ui/qt_dispatcher.cpp \
    $$IDE_DIR/ui/theme.cpp \
    $$IDE_DIR/ui/syntax_highlighter.cpp \
    $$IDE_DIR/ui/code_editor.cpp \
    $$IDE_DIR/ui/panels.cpp \
    $$IDE_DIR/ui/debug_panels.cpp \
    $$IDE_DIR/ui/templates.cpp \
    $$IDE_DIR/ui/find_bar.cpp \
    $$IDE_DIR/ui/main_window.cpp \
    $$IDE_DIR/ui/i18n.cpp \
    $$IDE_DIR/ui/app_shell.cpp \
    $$IDE_DIR/ui/hardware_designer.cpp \
    $$IDE_DIR/ui/document_tab.cpp \
    $$IDE_DIR/ui/block_painter.cpp \
    $$IDE_DIR/ui/block_scene.cpp \
    $$IDE_DIR/ui/block_view.cpp \
    $$IDE_DIR/ui/generated_code_pane.cpp \
    $$IDE_DIR/ui/block_editor.cpp \
    $$IDE_DIR/ui/comment_edit.cpp \
    $$IDE_DIR/ui/field_editor.cpp

# headers that declare Q_OBJECT classes (qmake runs moc on the HEADERS it is given)
UI_MOC_HEADERS = \
    $$IDE_DIR/ui/code_editor.h \
    $$IDE_DIR/ui/find_bar.h \
    $$IDE_DIR/ui/main_window.h \
    $$IDE_DIR/ui/app_shell.h \
    $$IDE_DIR/ui/hardware_designer.h \
    $$IDE_DIR/ui/block_view.h \
    $$IDE_DIR/ui/block_editor.h \
    $$IDE_DIR/ui/comment_edit.h \
    $$IDE_DIR/ui/field_editor.h \
    $$IDE_DIR/ui/generated_code_pane.h

# the translations (compiled .qm files are in the repository)
UI_RESOURCES = $$IDE_DIR/ui/translations/translations.qrc
