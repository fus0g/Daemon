# Development guide

## Requirements
Fedora-like Linux with Wayland. GCC 15/16 (C++23 + modules + `import std`), CMake >= 4.3, Ninja, pkg-config, dev packages
`wayland-client wayland-cursor xkbcommon vulkan-headers/loader`, `glslc` **or** `libshaderc_shared` (a fallback tool `tools/spvc.cpp`
compiles shaders). Runtime tools used by features: `clangd`, `gdb`, `git`, `zenity` (folder chooser), `cmake`, `ninja`.

## Build, run, test
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo     # once
cmake --build build                                                 # ~1 min clean, incremental is faster
./build/app/daemon [file|folder...]        # the IDE
./build/tests/daemon_tests                 # prints "all tests passed"; also `ctest --test-dir build`
```
After large restructurings delete `build/` and reconfigure (stale `.gcm` module files cause confusing "Bad file data" errors).
Filter output with `cmake --build build 2>&1 | grep -E " error|FAILED" -A5`.

Environment variables: `IDE_FONT`, `IDE_UI_FONT` (font files), `DAEMON_ASSETS` (asset dir), `IDE_GPU` (device index),
`IDE_VALIDATE=1` (Vulkan validation), `IDE_CLANGD` (clangd path), `IDE_LSP_TRACE=1` (LSP traffic to stderr),
`DAEMON_OPAQUE=1` (no translucent chrome), `DAEMON_ANIMATE=1` (animate in screenshot mode).

## Seeing the UI without interaction: screenshots and key scripts
```
./build/app/daemon --screenshot out.png --keys 'token|token|...' [folder-or-file ...]
```
Renders frames headlessly-ish (real Vulkan), replays the script, writes a 1280x800 PNG and exits. Read the PNG to inspect.
Tokens (implemented in `App::stepScript`, `app/impl/core.cpp`):
- keys: `ctrl+x`, `ctrl+shift+n`, `alt+x`, `enter esc tab backspace delete left right up down home end pgup pgdn f2 f3 f5 f9 f10 f12`
  (a backtick is `ctrl+\``); text: `t:hello` types characters.
- timing: `sleep:ms`, `wait` (until idle), `waitrun` (until the program runs), `dbgwait` (until build done and debugger paused).
  NB: a script is cut off after ~2400 frames, so keep sleeps short (a few seconds in total).
- UI hooks: `side` focus explorer, `menu` context menu, `filedrag`, `dragdemo:left|right|up|down|tabs`, `tooltip:N`, `gitsub:N`,
  `gitexpand:N`, `blamehover`, `panelsel`, `hoverfake`, `panel:N` (bottom tab), `debugview`, `search:text`, `replaceall:text`.
- debugger hooks: `bp:LINE`, `bpedit:LINE:text`, `dbgwatch:expr`, `dbgframe:N`, `dbgexpand:path`, `dbgeval:expr`.
Examples:
```
--keys 'ctrl+shift+p|sleep:600'                                         # welcome dialog
--keys 'ctrl+`|sleep:1500|t:ls --color|enter|sleep:1500'                 # integrated terminal
--keys 'bp:8|f5|sleep:500|dbgwait|sleep:1200|dbgeval:v*2' proj proj/main.cpp   # debugger session on a CMake project
--keys 'search:termFocus_|sleep:2000' .                                  # project search
```
Real mouse input cannot be scripted: pointer features (drag and drop, text selection, hover buttons) are verified by reading the
code and by screenshots of forced states only. Say so in your report.

## Tests
`tests/test_{base,text,ui,services}.cpp` + `test_main.cpp`; harness `tests/check.hpp` (`CHECK(cond)`). They are headless:
buffer/undo, highlighter, document editing, JSON, process + PTY, LSP against `tests/mock_lsp.py`, CMake parsing and module scan,
projects, git (scratch repos), terminal emulator, search/replace, gdb MI parsing plus a live gdb session (skipped if gdb/g++ is
missing), icons, file operations. Add a test function to the matching file and call it from that file's `run<Group>Tests()`.

## Conventions
- C++23, `import std;`; no exceptions across module boundaries except Vulkan init; `std::expected` for fallible APIs.
- Modules are `daemon.<lib>.<name>`; C++ namespace is `ide` (a namespace called `daemon` clashes with `unistd.h`).
- A module = `x.cppm` (interface: types, declarations, short inline functions) + `x.cpp` (`module daemon.x.y;`, long bodies).
  Keep an interface lean; move bodies > ~5 lines to the `.cpp`. Defaults for parameters live only in the declaration.
- `App` members: declare in `app/app.cppm` (data members and short inline helpers also live there), define in `app/impl/<feature>.cpp`
  (`module daemon.app;` + the same import list). New feature file -> add to `app/CMakeLists.txt`.
- Layering: `app -> services, ui, text -> base`. `libs/ui` must never import IDE modules.
- Do not use `std::regex` in hot or module-scanning code (it was removed from CMake handling); `search` uses it deliberately.
- Style: 4 spaces, `lowerCamel` functions and members, trailing `_` for private data, comments explain *why*.

## Recipes
- **Add a keyboard shortcut**: `App::editorKeyImpl` (`impl/keyboard.cpp` / `impl/prompts.cpp` area) for editor shortcuts;
  debugger keys in `debugKey`; global ones early in `onKey`. Document it in `docs/FEATURES.md`.
- **Add an activity-bar button**: ids and positions in `activityPos`, glyph table in `drawActivityBar`, behaviour in
  `activityClick/activityActive` (`impl/activity_bar.cpp`); a lone button gets its own circular pill (`activityPill`).
- **Add a bottom-panel tab**: extend the arrays (`panelScroll_`, `panelFollow_`, `panelTabRects_`), the names list in `drawPanel`,
  body drawing and click/key routing in `impl/build_run.cpp` (panel code) and `onKey`.
- **Add a sidebar view**: a `sideX_` flag, include it in the explorer-exclusion checks (`!sideGit_ && !sideSearch_ && !sideDebug_`),
  draw in `drawSidebar`, click in `onPointerButton`, scroll in `onScroll`.
- **Add a service**: new module in `libs/services` (+ `.cpp`), register both files in its `CMakeLists.txt`, test in
  `tests/test_services.cpp`, glue code in a new `app/impl/*.cpp`.
- **Async work**: `spawn(work)` runs on a thread and returns a lambda applied on the UI thread by `pollJobs`; subprocess output is
  pumped by `win_.watch(fd, callback)`. Never touch `App` state from the worker.
- **Drawing**: build quads through `fill(...)`, `uiLabel(...)`, `Icons::glyph(...)`, `glassPanel(...)`; coordinates are physical
  pixels (`S()` is the scale factor; multiply design numbers by `s`).

## Compiler quirks found (GCC 16)
- `app/main.cpp` must not `import daemon.app` (BMI read failure with `std::println`); it calls `extern "C" int daemon_main` in `cli.cpp`.
- A defaulted `friend operator<=>` in a module ICE'd; `Pos` uses member operators.
- A constructor with a long member-init list inside a module interface plus certain regex code produced unreadable BMIs earlier;
  the `search::Matcher` constructor therefore lives in `search.cpp`.
- If you see `failed to read compiled module cluster`, clean the build directory first before debugging further.

## Performance notes
- Measure: `IDE_BENCH=500 ./build/app/daemon [file]` prints average draw and render CPU per frame (≈0.05 ms / 0.5 ms).
- GPU: the integrated GPU is preferred (power, memory, no dGPU wake-up); `IDE_GPU=<n>` forces a device.
- Present mode is MAILBOX when available and frames are paced by `wl_surface.frame` callbacks; a hidden window stops rendering.
- clangd runs niced, `-j=2`, PCH on disk. Background diagnostics keep a rolling window of 8 files open (`bgSettled_`) and wait for clangd.
- The caret stops blinking after 30 s idle so an idle editor schedules no frames. Git status polls every 8 s while focused.
- `IDE_BGINDEX=0` disables clangd background indexing.
