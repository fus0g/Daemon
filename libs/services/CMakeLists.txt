add_library(daemon_services STATIC)
target_sources(daemon_services
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    cmake.cppm
    explorer.cppm
    gdb.cppm
    git.cppm
    lsp.cppm
    notebook.cppm
    project.cppm
    search.cppm
    terminal.cppm
    toolchain.cppm
  PRIVATE
    cmake.cpp
    explorer.cpp
    gdb.cpp
    git.cpp
    lsp.cpp
    notebook.cpp
    project.cpp
    search.cpp
    terminal.cpp
    toolchain.cpp)
target_link_libraries(daemon_services PUBLIC daemon_base)
target_link_libraries(daemon_services PRIVATE daemon_warnings)
