# Changelog

All notable changes to Daemon are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project uses [Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).

## [Unreleased]

## [0.1.0] - 2026-11-11

First tagged release.

### Added
- Native Wayland + Vulkan 1.3 window with translucent, blurred chrome; layered C++23 module code base (`libs/` + `app/`).
- Editor: tabs, split panes, drag and drop, minimap, breadcrumbs, syntax highlighting, find/replace, animated caret.
- C/C++ intelligence through clangd, including C++20 modules (`import std;`), project-wide problems.
- CMake projects: configure, build, run, profiles, targets, clickable compiler errors.
- gdb debugger: breakpoints (conditional, logpoints, data, function, C++ throw), stepping, variables, watches, call stack, console.
- Git integration, project-wide search and replace, integrated terminal, file explorer with drag-to-move and live updates.
- Welcome screen, new-project wizard, system folder chooser.
- Toolchain check at start-up with distro-specific install commands and verified one-click downloads of clangd, CMake and Ninja.
- Dragon-winged helm logo, launcher entry and icons registered automatically.
- GPL-3.0-or-later licence, third-party notices, handoff and architecture documentation.

### Performance
- Prefers the integrated GPU, frame-paced presentation, bounded clangd resource use, idle editor does not wake up.

[Unreleased]: https://example.invalid/compare/v0.1.0...HEAD
[0.1.0]: https://example.invalid/releases/tag/v0.1.0
