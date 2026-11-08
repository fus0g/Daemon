
# ---- Wayland protocols (generated C glue + headers)
pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
if(NOT WAYLAND_SCANNER)
  find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
endif()
set(PROTO_DIR ${PROJECT_SOURCE_DIR}/third_party/wayland)
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
add_library(daemon_protocols STATIC ${PROTO_SOURCES})
target_include_directories(daemon_protocols PUBLIC ${GEN_DIR})
target_link_libraries(daemon_protocols PUBLIC PkgConfig::WAYLAND_CLIENT)
set_target_properties(daemon_protocols PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---- Shaders: glslc when present, otherwise a tiny compiler on top of libshaderc
find_program(GLSLC glslc)
if(NOT GLSLC)
  find_library(SHADERC_LIB NAMES shaderc_shared libshaderc_shared.so.1
               PATHS /usr/lib64 /usr/lib /usr/lib/x86_64-linux-gnu)
  if(NOT SHADERC_LIB)
    message(FATAL_ERROR "Need glslc (shaderc / glslang tools) or libshaderc_shared to compile shaders.")
  endif()
  add_executable(spvc ${PROJECT_SOURCE_DIR}/tools/spvc.cpp)
  set_target_properties(spvc PROPERTIES CXX_MODULE_STD OFF)
  target_link_libraries(spvc PRIVATE ${SHADERC_LIB})
endif()
set(SHADER_INCS)
foreach(sh quad.vert quad.frag glass.frag)
  set(in  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh})
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
add_custom_target(daemon_shaders DEPENDS ${SHADER_INCS})

# ---- the UI library itself
add_library(daemon_ui STATIC)
target_sources(daemon_ui
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    draw.cppm
    font.cppm
    geometry.cppm
    icons.cppm
    input.cppm
    vulkan.cppm
    wayland.cppm
  PRIVATE
    draw.cpp
    font.cpp
    icons.cpp
    stb_impl.cpp
    vulkan.cpp
    wayland.cpp)
target_link_libraries(daemon_ui PUBLIC daemon_base)
target_link_libraries(daemon_ui PRIVATE daemon_warnings)
target_link_libraries(daemon_ui PUBLIC daemon_protocols Vulkan::Vulkan
  PkgConfig::WAYLAND_CLIENT PkgConfig::WAYLAND_CURSOR PkgConfig::XKBCOMMON)
add_dependencies(daemon_ui daemon_shaders)
set_source_files_properties(vulkan.cppm PROPERTIES OBJECT_DEPENDS "${SHADER_INCS}")
target_include_directories(daemon_ui PRIVATE ${GEN_DIR} ${PROJECT_SOURCE_DIR}/third_party/stb)
target_compile_definitions(daemon_ui PRIVATE DAEMON_ASSET_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
set_source_files_properties(stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
