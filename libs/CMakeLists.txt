# Daemon is built from layered static libraries (each a set of C++20 modules):
#   base      utilities, JSON, child processes
#   text      text buffer, syntax highlighting, document model
#   ui        window / renderer framework: Wayland, Vulkan, drawing, fonts, icons   (no IDE logic)
#   services  language server, CMake, git, gdb, search, terminal emulator, projects, file tree
add_subdirectory(base)
add_subdirectory(text)
add_subdirectory(ui)
add_subdirectory(services)
