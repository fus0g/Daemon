# The application library (the IDE) and the `daemon` executable.
add_library(daemon_app STATIC)
target_sources(daemon_app
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    app.cppm
    options.cppm
    theme.cppm
  PRIVATE
    app_activity_bar.cpp
    app_build_run.cpp
    app_core.cpp
    app_debugger.cpp
    app_dialogs.cpp
    app_documents.cpp
    app_editor_view.cpp
    app_file_ops.cpp
    app_git_view.cpp
    app_keyboard.cpp
    app_layout.cpp
    app_lsp.cpp
    app_pointer.cpp
    app_prompts.cpp
    app_render.cpp
    app_search_view.cpp
    app_terminal_view.cpp
    app_ui_helpers.cpp
    cli.cpp)
target_link_libraries(daemon_app PUBLIC daemon_base daemon_text daemon_ui daemon_services PRIVATE daemon_warnings)

add_executable(daemon main.cpp)
target_link_libraries(daemon PRIVATE daemon_app)
