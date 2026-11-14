cmake_minimum_required(VERSION 4.3)

# `import std;` is still gated behind an experimental flag in CMake 4.x.
# The value below is the gate for CMake 4.3 ("CxxImportStd") and has to be set
# before the first project() call.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)

project(daemon VERSION 0.3.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "Daemon is Linux-only (Wayland + Vulkan).")
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

set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)   # generated protocol headers and compiled shaders
file(MAKE_DIRECTORY ${GEN_DIR})

# Warning flags shared by every Daemon library.
add_library(daemon_warnings INTERFACE)
target_compile_options(daemon_warnings INTERFACE -Wall -Wextra -Wno-missing-field-initializers)

add_subdirectory(libs)      # base, text, ui (window + renderer framework), services
add_subdirectory(app)       # the IDE: application library + `daemon` executable

enable_testing()
add_subdirectory(tests)

# Desktop integration (cmake --install build --prefix ~/.local)
install(FILES assets/daemon.svg DESTINATION share/icons/hicolor/scalable/apps)
foreach(px 48 64 128 256 512)
  install(FILES assets/icons/${px}x${px}/daemon.png DESTINATION share/icons/hicolor/${px}x${px}/apps)
endforeach()
install(FILES assets/daemon.desktop DESTINATION share/applications)

# Distribution packages -> dist/  (cmake --build build --target package-deb | package-rpm | packages)
foreach(fmt deb rpm)
  add_custom_target(package-${fmt}
    COMMAND ${PROJECT_SOURCE_DIR}/tools/package.sh ${fmt}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} USES_TERMINAL VERBATIM
    COMMENT "Building the ${fmt} package")
endforeach()
add_custom_target(packages
  COMMAND ${PROJECT_SOURCE_DIR}/tools/package.sh all
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} USES_TERMINAL VERBATIM
  COMMENT "Building deb and rpm packages")
