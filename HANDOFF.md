# Daemon — project handoff

**Daemon** is a Linux-only IDE written from scratch in C++23/C++20 modules: native Wayland client, Vulkan 1.3 renderer, no UI toolkit.
It aims at the CLion / VS Code / Zed experience: editor, CMake projects, clangd intelligence (incl. C++20 modules), git, search,
integrated terminal, debugger — all in a dark "island" UI with Material icons and a Targaryen-crimson accent.

Licence: GPL-3.0-or-later (see `LICENSE`).

Repository: `/home/fus0g/Projects/ide` (git, default branch, no remote). The owner (Fedora, GNOME Wayland, NVIDIA RTX 3050 Ti) drives
the work by asking for features/redesigns, then reviewing screenshots; expect short, visual feedback and frequent "revert it".

## Read in this order
1. `AGENTS.md` — ground rules and document index.
2. `docs/DEVELOPMENT.md` — build, tests, screenshot scripting, recipes, GCC quirks.
3. `docs/ARCHITECTURE.md` — libraries, modules, `App` layout, frame loop.
4. `docs/SUBSYSTEMS.md` — how clangd/CMake, git, gdb, search, terminal, file watching work.
5. `docs/FEATURES.md`, `docs/DESIGN.md`, `docs/KNOWN_ISSUES.md`.
User-facing description: `README.md` (overview) and `docs/USER_GUIDE.md` (full feature reference).

## State at handoff
- Everything is committed; `cmake --build build` succeeds and `./build/tests/daemon_tests` prints `all tests passed`.
- Latest large change: the code base was restructured into layered libraries (`libs/{base,text,ui,services}` + `app/`), every module
  split into `.cppm` interface + `.cpp` implementation, `App` implementation split into `app/impl/*.cpp` by feature, tests split per
  library. Behaviour was re-checked afterwards with screenshots (debugger session, search, terminal, startup) and unit tests.
- Features done: editor/tabs/splits/minimap, explorer + file operations + drag move, clangd (modules aware), CMake build/run, git
  (gutter, blame, history, branches), project search & replace, integrated terminal, gdb debugger, project dialogs, live file watching,
  system folder chooser, glassy chrome with the final tint `#111218` @ 214.
- Build layout: binaries are `build/app/daemon` and `build/tests/daemon_tests`.

## Build and verify in 30 seconds
```
cmake -S . -B build -G Ninja && cmake --build build
./build/tests/daemon_tests
./build/app/daemon --screenshot /tmp/shot.png --keys 'ctrl+shift+p|sleep:600'   # then read the PNG
```

## Things to know before touching code
- C++ namespace is `ide`, modules are `daemon.<lib>.<name>`; dependencies point downward (`app -> services/ui/text -> base`).
- New `App` behaviour: declare in `app/app.cppm`, define in the matching `app/impl/*.cpp`.
- The UI is immediate mode (`App::draw` rebuilds a quad list every frame); hit-testing recomputes the same rectangles.
- Async work never touches `App` state off the UI thread (`spawn` + `pollJobs`; subprocess fds via `win_.watch`).
- Don't call `dbgEnd` from inside a gdb callback (use `dbgScheduleEnd`); clear callbacks before destroying objects they capture.
- Rejected visuals are listed in `docs/DESIGN.md`; check it before restyling anything.
- GCC 16 module bugs shaped some code (`app/main.cpp` entry through `daemon_main`, `Pos` operators, `search::Matcher` ctor in a `.cpp`);
  if module files look corrupt, delete `build/` and reconfigure.

## Open items (details in `docs/KNOWN_ISSUES.md`)
Hand-test the pointer-driven features (explorer drag/drop, terminal selection, debugger hover buttons, search replace buttons),
confirm the welcome-screen flicker report is gone, then consider: breakpoint line tracking on edits, in-editor find overlay,
clone-from-URL, terminal tabs, disassembly/registers views, decomposing `App` into feature classes.
