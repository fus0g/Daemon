# The application library (the IDE) and the `daemon` executable.
add_library(daemon_app STATIC)
target_sources(daemon_app
  PUBLIC FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES
    app.cppm
    options.cppm
    theme.cppm
  PRIVATE
    cli.cpp
    options.cpp
    impl/activity_bar.cpp
    impl/build_run.cpp
    impl/core.cpp
    impl/desktop.cpp
    impl/setup.cpp
    impl/session.cpp
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
# Version: the single source of truth is project(VERSION) in the top-level CMakeLists.txt.
# Development builds also carry the git revision ("0.1.0+3f2a1bc", "+dirty" for uncommitted changes).
set(DAEMON_VERSION_FULL "${PROJECT_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v${PROJECT_VERSION}"
                  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE _exact ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE _rev ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE _dirty ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_rev AND NOT _exact)
    set(DAEMON_VERSION_FULL "${PROJECT_VERSION}+${_rev}")
  endif()
  if(_dirty)
    set(DAEMON_VERSION_FULL "${DAEMON_VERSION_FULL}.dirty")
  endif()
endif()
set_source_files_properties(options.cpp PROPERTIES COMPILE_DEFINITIONS "DAEMON_VERSION=\"${PROJECT_VERSION}\";DAEMON_VERSION_FULL=\"${DAEMON_VERSION_FULL}\"")
target_link_libraries(daemon_app PUBLIC daemon_base daemon_text daemon_ui daemon_services PRIVATE daemon_warnings)

add_executable(daemon main.cpp compat.c)
set_source_files_properties(compat.c PROPERTIES COMPILE_OPTIONS "-fno-builtin-sqrt;-fno-builtin-acos;-O1")
target_link_libraries(daemon PRIVATE daemon_app)
# Portability: needs glibc >= 2.38 only (see compat.c). The GCC runtime this was built with is bundled by the packages
# (tools/package.sh) in <prefix>/lib/daemon, which the executable finds through its RUNPATH, so it also runs on distributions
# that ship an older libstdc++. In a plain build tree that directory does not exist and the system libraries are used.
target_link_options(daemon PRIVATE "LINKER:-rpath,$ORIGIN/../lib/daemon" -Wl,--wrap=sqrtf -Wl,--wrap=acosf)
install(TARGETS daemon RUNTIME DESTINATION bin)
