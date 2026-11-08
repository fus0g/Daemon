add_library(daemon_text STATIC)
target_sources(daemon_text
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    buffer.cppm
    document.cppm
    syntax.cppm
  PRIVATE
    buffer.cpp
    document.cpp
    syntax.cpp)
target_link_libraries(daemon_text PUBLIC daemon_base)
target_link_libraries(daemon_text PRIVATE daemon_warnings)
