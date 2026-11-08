# The application library (the IDE) and the `daemon` executable.
add_library(daemon_app STATIC)
target_sources(daemon_app
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    app.cppm
    options.cppm
    theme.cppm
  PRIVATE
    cli.cpp)
target_link_libraries(daemon_app PUBLIC daemon_base daemon_text daemon_ui daemon_services PRIVATE daemon_warnings)

add_executable(daemon main.cpp)
target_link_libraries(daemon PRIVATE daemon_app)
