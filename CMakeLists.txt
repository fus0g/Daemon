cmake_minimum_required(VERSION 4.3)

# `import std;` is still gated behind an experimental flag in CMake 4.x.
# The value below is the gate for CMake 4.3 ("CxxImportStd") and has to be set
# before the first project() call.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)

project(ide VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "ide is Linux-only (Wayland + Vulkan).")
endif()
if(NOT CMAKE_GENERATOR MATCHES "Ninja")
  message(FATAL_ERROR "C++ modules need a Ninja generator: cmake -G Ninja ...")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Vulkan REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_CURSOR REQUIRED IMPORTED_TARGET wayland-cursor)
pkg_check_modules(XKBCOMMON      REQUIRED IMPORTED_TARGET xkbcommon)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER)
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
endif()

set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

# ---------------------------------------------------------------- Wayland protocols
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/third_party/wayland)
set(PROTOCOLS xdg-shell xdg-decoration-unstable-v1 ext-background-effect-v1)
set(PROTO_SOURCES)
foreach(p IN LISTS PROTOCOLS)
  set(xml ${PROTO_DIR}/${p}.xml)
  add_custom_command(OUTPUT ${GEN_DIR}/${p}-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${GEN_DIR}/${p}-protocol.c
    DEPENDS ${xml} VERBATIM)
  add_custom_command(OUTPUT ${GEN_DIR}/${p}-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${GEN_DIR}/${p}-client-protocol.h
    DEPENDS ${xml} VERBATIM)
  list(APPEND PROTO_SOURCES ${GEN_DIR}/${p}-protocol.c ${GEN_DIR}/${p}-client-protocol.h)
endforeach()
add_library(ide_protocols STATIC ${PROTO_SOURCES})
target_include_directories(ide_protocols PUBLIC ${GEN_DIR})
target_link_libraries(ide_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)
set_target_properties(ide_protocols PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- Shaders
# Use glslc if present, otherwise build a tiny compiler on top of libshaderc.
find_program(GLSLC glslc)
if(NOT GLSLC)
  find_library(SHADERC_LIB NAMES shaderc_shared libshaderc_shared.so.1
               PATHS /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu)
  if(NOT SHADERC_LIB)
    message(FATAL_ERROR "Need glslc (shaderc / glslang tools) or libshaderc_shared to compile shaders.")
  endif()
  add_executable(spvc tools/spvc.cpp)
  set_target_properties(spvc PROPERTIES CXX_MODULE_STD OFF)
  target_link_libraries(spvc PRIVATE ${SHADERC_LIB})
endif()

set(SHADER_INCS)
foreach(sh quad.vert quad.frag glass.frag)
  set(in  ${CMAKE_SOURCE_DIR}/shaders/${sh})
  set(out ${GEN_DIR}/${sh}.inc)
  if(GLSLC)
    add_custom_command(OUTPUT ${out}
      COMMAND ${GLSLC} --target-env=vulkan1.3 -mfmt=c -o ${out} ${in}
      DEPENDS ${in} VERBATIM)
  else()
    add_custom_command(OUTPUT ${out}
      COMMAND spvc ${in} ${out}
      DEPENDS ${in} spvc VERBATIM)
  endif()
  list(APPEND SHADER_INCS ${out})
endforeach()
add_custom_target(ide_shaders DEPENDS ${SHADER_INCS})

# ---------------------------------------------------------------- Core library (modules)
add_library(ide_core STATIC src/stb_impl.cpp)
target_sources(ide_core PUBLIC FILE_SET CXX_MODULES BASE_DIRS src FILES
  src/util.cppm
  src/json.cppm
  src/process.cppm
  src/cmake.cppm
  src/project.cppm
  src/lsp.cppm
  src/text.cppm
  src/syntax.cppm
  src/input.cppm
  src/font.cppm
  src/draw.cppm
  src/icons.cppm
  src/vulkan.cppm
  src/wayland.cppm
  src/editor.cppm
  src/explorer.cppm
  src/app.cppm)
add_dependencies(ide_core ide_shaders)
set_source_files_properties(src/vulkan.cppm PROPERTIES OBJECT_DEPENDS "${SHADER_INCS}")
target_include_directories(ide_core PRIVATE ${GEN_DIR} third_party/stb)
target_compile_definitions(ide_core PRIVATE DAEMON_ASSET_DIR="${CMAKE_SOURCE_DIR}/third_party/fonts")
target_link_libraries(ide_core PUBLIC ide_protocols Vulkan::Vulkan
  PkgConfig::WAYLAND_CLIENT PkgConfig::WAYLAND_CURSOR PkgConfig::XKBCOMMON)
target_compile_options(ide_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_source_files_properties(src/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

add_executable(ide src/main.cpp)
set_target_properties(ide PROPERTIES OUTPUT_NAME daemon)
target_link_libraries(ide PRIVATE ide_core)

enable_testing()
add_executable(ide_tests tests/tests.cpp)
target_link_libraries(ide_tests PRIVATE ide_core)
target_compile_definitions(ide_tests PRIVATE IDE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ide_tests COMMAND ide_tests)
