cmake_minimum_required(VERSION 3.16)
project(AssemblyCore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# The core is standard C++17 only: no platform API, no file system access, no third-party library.
add_library(asmcore STATIC
  core/src/util.cpp
  core/src/profile.cpp
  core/src/assemble.cpp
  core/src/lower.cpp
  core/src/layout.cpp
  core/src/dataflow.cpp
  core/src/lang.cpp
  core/src/index.cpp
  core/src/sram.cpp
  core/src/artifacts.cpp
  core/src/events.cpp
  core/src/diag_names.cpp
  core/src/hardware.cpp
  core/src/profile_info.cpp
  core/src/blocks.cpp
  core/src/blocks_import.cpp
  core/src/blocks_values.cpp
  core/src/blocks_symbols.cpp
  core/src/blocks_edit.cpp
  core/src/report.cpp
  core/src/image_view.cpp
  core/src/disasm.cpp
  core/src/sim_params.cpp
  core/src/sim/ini.cpp
  core/src/sim/sexpr.cpp
  core/src/sim/impl.cpp
  core/src/sim/env.cpp
  core/src/sim/builtin.cpp
  core/src/sim/run.cpp
  core/src/sim/main_core.cpp
  core/src/sim/peri_core.cpp
  core/src/sim/debug.cpp
  core/src/sim/scenario.cpp)
target_include_directories(asmcore PUBLIC core/include)

# Writes docs/SIM_PARAMS.md from the parameter catalog:  gen_sim_params_doc > docs/SIM_PARAMS.md
add_executable(gen_sim_params_doc cli/gen_sim_params_doc.cpp)
target_link_libraries(gen_sim_params_doc PRIVATE asmcore)
if(MSVC)
  target_compile_options(gen_sim_params_doc PRIVATE /utf-8)      # the text it writes is Chinese
endif()

# Command-line assembler.
add_executable(asmc cli/main.cpp)
target_link_libraries(asmc PRIVATE asmcore)

# Disassembler front end.
add_executable(asmdis cli/dis_main.cpp)
target_link_libraries(asmdis PRIVATE asmcore)

# Simulator front end: assemble and run.
add_executable(asmsim cli/sim_main.cpp)
target_link_libraries(asmsim PRIVATE asmcore)

# Hardware Designer on the command line.
add_executable(asmhw cli/hw_main.cpp)
target_link_libraries(asmhw PRIVATE asmcore)

# Block programs on the command line.
add_executable(asmblk cli/blk_main.cpp)
target_link_libraries(asmblk PRIVATE asmcore)

# Tests (no framework: plain assertions, like the AutoDetectDevice test program).
enable_testing()
add_executable(asmcore_tests tests/main.cpp)
target_link_libraries(asmcore_tests PRIVATE asmcore)
target_compile_definitions(asmcore_tests PRIVATE ASMCORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(asmcore_tests PRIVATE core/src)
add_test(NAME asmcore_tests COMMAND asmcore_tests)

add_executable(asmcore_sim_tests tests/sim_tests.cpp)
target_link_libraries(asmcore_sim_tests PRIVATE asmcore)
target_compile_definitions(asmcore_sim_tests PRIVATE ASMCORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME asmcore_sim_tests COMMAND asmcore_sim_tests)

add_executable(asmcore_disasm_tests tests/disasm_tests.cpp)
target_link_libraries(asmcore_disasm_tests PRIVATE asmcore)
target_compile_definitions(asmcore_disasm_tests PRIVATE ASMCORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME asmcore_disasm_tests COMMAND asmcore_disasm_tests)

add_executable(asmcore_hardware_tests tests/hardware_tests.cpp)
target_link_libraries(asmcore_hardware_tests PRIVATE asmcore)
target_compile_definitions(asmcore_hardware_tests PRIVATE ASMCORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME asmcore_hardware_tests COMMAND asmcore_hardware_tests)

add_executable(asmcore_blocks_tests tests/blocks_tests.cpp)
target_link_libraries(asmcore_blocks_tests PRIVATE asmcore)
target_compile_definitions(asmcore_blocks_tests PRIVATE ASMCORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME asmcore_blocks_tests COMMAND asmcore_blocks_tests)

add_executable(asmcore_blocks_components_tests tests/blocks_components_tests.cpp)
target_link_libraries(asmcore_blocks_components_tests PRIVATE asmcore)
target_compile_definitions(asmcore_blocks_components_tests PRIVATE ASMCORE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME asmcore_blocks_components_tests COMMAND asmcore_blocks_components_tests)

add_executable(asmcore_blocks_edit_tests tests/blocks_edit_tests.cpp)
target_link_libraries(asmcore_blocks_edit_tests PRIVATE asmcore)
add_test(NAME asmcore_blocks_edit_tests COMMAND asmcore_blocks_edit_tests)

# The sample projects (sample/) double as an integration test: build, run the scenarios, disassemble and compare, with the tools just built
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_FOUND AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sample/check_samples.py)
  add_test(NAME sample_projects COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sample/check_samples.py --bin-dir $<TARGET_FILE_DIR:asmc>)
endif()

# The IDE (services + Qt application). Off in a pure core build: cmake -DASMCORE_BUILD_IDE=OFF
option(ASMCORE_BUILD_IDE "Build AssemblyCoreIDE (services, and the Qt application if Qt6 is found)" ON)
if(ASMCORE_BUILD_IDE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/AssemblyCoreIDE/CMakeLists.txt)
  add_subdirectory(AssemblyCoreIDE)
endif()
