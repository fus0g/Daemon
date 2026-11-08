add_executable(daemon_tests tests.cpp)
target_link_libraries(daemon_tests PRIVATE daemon_app)
target_compile_definitions(daemon_tests PRIVATE DAEMON_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME daemon_tests COMMAND daemon_tests)
