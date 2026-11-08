add_library(daemon_services STATIC)
target_sources(daemon_services
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    lsp.cppm
    cmake.cppm
    git.cppm
    gdb.cppm
    search.cppm
    terminal.cppm
    project.cppm
    explorer.cppm)
target_link_libraries(daemon_services PUBLIC daemon_base)
target_link_libraries(daemon_services PRIVATE daemon_warnings)
