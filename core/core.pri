# qmake counterpart of the asmcore library in the top-level CMakeLists.txt (tests/check_build_files.py keeps the lists equal).
# Standard C++17 only. Include from a .pro file located in a sibling directory of core/.
ASMCORE_DIR = $$PWD
INCLUDEPATH += $$ASMCORE_DIR/include $$ASMCORE_DIR/src
CORE_SOURCES = \
    $$ASMCORE_DIR/src/util.cpp \
    $$ASMCORE_DIR/src/profile.cpp \
    $$ASMCORE_DIR/src/assemble.cpp \
    $$ASMCORE_DIR/src/lower.cpp \
    $$ASMCORE_DIR/src/layout.cpp \
    $$ASMCORE_DIR/src/dataflow.cpp \
    $$ASMCORE_DIR/src/lang.cpp \
    $$ASMCORE_DIR/src/index.cpp \
    $$ASMCORE_DIR/src/sram.cpp \
    $$ASMCORE_DIR/src/artifacts.cpp \
    $$ASMCORE_DIR/src/events.cpp \
    $$ASMCORE_DIR/src/diag_names.cpp \
    $$ASMCORE_DIR/src/hardware.cpp \
    $$ASMCORE_DIR/src/profile_info.cpp \
    $$ASMCORE_DIR/src/blocks.cpp \
    $$ASMCORE_DIR/src/blocks_import.cpp \
    $$ASMCORE_DIR/src/report.cpp \
    $$ASMCORE_DIR/src/image_view.cpp \
    $$ASMCORE_DIR/src/disasm.cpp \
    $$ASMCORE_DIR/src/sim/ini.cpp \
    $$ASMCORE_DIR/src/sim/sexpr.cpp \
    $$ASMCORE_DIR/src/sim/impl.cpp \
    $$ASMCORE_DIR/src/sim/env.cpp \
    $$ASMCORE_DIR/src/sim/builtin.cpp \
    $$ASMCORE_DIR/src/sim/run.cpp \
    $$ASMCORE_DIR/src/sim/main_core.cpp \
    $$ASMCORE_DIR/src/sim/peri_core.cpp \
    $$ASMCORE_DIR/src/sim/debug.cpp \
    $$ASMCORE_DIR/src/sim/scenario.cpp
CONFIG += c++17
