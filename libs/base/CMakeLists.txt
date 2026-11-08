add_library(daemon_base STATIC)
target_sources(daemon_base
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    util.cppm
    json.cppm
    process.cppm)
target_link_libraries(daemon_base PRIVATE daemon_warnings)
