# The application library (the IDE) and the `daemon` executable.
add_library(daemon_app STATIC)
target_sources(daemon_app
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    app.cppm
    options.cppm
    theme.cppm
  PRIVATE
    cli.cpp
    impl/activity_bar.cpp
    impl/build_run.cpp
    impl/core.cpp
    impl/desktop.cpp
    impl/setup.cpp
    impl/debugger.cpp
    impl/dialogs.cpp
    impl/documents.cpp
    impl/editor_view.cpp
    impl/file_ops.cpp
    impl/git_view.cpp
    impl/keyboard.cpp
    impl/layout.cpp
    impl/lsp.cpp
    impl/pointer.cpp
    impl/prompts.cpp
    impl/render.cpp
    impl/search_view.cpp
    impl/terminal_view.cpp
    impl/ui_helpers.cpp)
target_link_libraries(daemon_app PUBLIC daemon_base daemon_text daemon_ui daemon_services PRIVATE daemon_warnings)

add_executable(daemon main.cpp)
target_link_libraries(daemon PRIVATE daemon_app)
install(TARGETS daemon RUNTIME DESTINATION bin)
