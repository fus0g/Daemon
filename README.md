# Daemon

A code editor for Linux: native **Wayland** client, **Vulkan 1.3** renderer, written in C++23 with
**C++20 modules** (`import std;` throughout).

## Build & run

Requires GCC 15+ (or Clang + libc++), CMake 4.3+, Ninja, and the dev packages for
`wayland-client`, `wayland-cursor`, `xkbcommon`, Vulkan headers/loader, plus `glslc` **or** `libshaderc_shared`
(shaders are compiled at build time).

    cmake -S . -B build -G Ninja
    cmake --build build
    ./build/daemon [file...]
    ctest --test-dir build        # headless unit tests

Options: `--font-size <px>`, `--screenshot out.png` (render, save PNG, exit), `--keys "ctrl+f|t:text|enter"`
(replay input). Environment: `IDE_FONT=<ttf/otf>` (code font), `IDE_UI_FONT=<ttf/otf>` (interface font), `IDE_GPU=<index>`, `IDE_VALIDATE=1`.

## Layout

| module         | role                                                              |
|----------------|-------------------------------------------------------------------|
| `ide.util`     | UTF-8, file I/O, PNG writer                                       |
| `ide.text`     | line buffer with grouped / coalescing undo-redo                   |
| `ide.syntax`   | table-driven highlighter (C/C++, Python, Rust, JS/TS, Go, ...)    |
| `ide.editor`   | document: cursor, selection, editing commands, find, brackets     |
| `ide.json`     | JSON value / parser / serialiser (LSP, CMake File API)            |
| `ide.process`  | child processes with non-blocking pipes, process-group kill        |
| `ide.cmake`    | profiles, configure/build argv, File API targets, error parsing   |
| `ide.lsp`      | Language Server Protocol client (clangd)                           |
| `ide.explorer` | file-tree model for the sidebar                                   |
| `ide.font`     | system font discovery, stb_truetype rasteriser, glyph atlas       |
| `ide.icons`    | vector file/folder icons rasterised into the atlas               |
| `ide.draw`     | colours + per-frame instanced quad list                           |
| `ide.vulkan`   | swapchain, one pipeline, atlas upload, screenshot readback        |
| `ide.wayland`  | xdg-shell window, xkbcommon keys, clipboard, cursors, HiDPI scale |
| `ide.app`      | tabs, prompts, input routing, drawing, frame loop                 |

## Keys

`Ctrl+O/N/S/Shift+S/W/Q` open/new/save/save as/close tab/quit - `Ctrl+Shift+O` open folder, `Ctrl+B` toggle sidebar, `Ctrl+Shift+E` focus it (arrows, Enter, `F5` refresh; drag the gap to resize) - `Ctrl+F/H/G` find/replace/go to line (`F3` next) -
`Ctrl+Z/Y` undo/redo - `Ctrl+/` comment - `Ctrl+Shift+D/K` duplicate/delete line - `Alt+Up/Down` move line -
`Ctrl+]/[` indent - `Ctrl+Tab`, `Ctrl+PgUp/PgDn` switch tab - `Ctrl +/-/0` or `Ctrl+wheel` zoom.

### File explorer

Right-click a row (or the empty area) for the context menu. With the sidebar focused:
`Ctrl+N` / `Ctrl+Shift+N` new file / folder (nested paths like `a/b.txt` work) - `F2` rename -
`Ctrl+C` / `Ctrl+X` / `Ctrl+V` copy / cut / paste - `Ctrl+D` duplicate - `Del` move to Trash (freedesktop
spec, restorable from your file manager), `Shift+Del` delete permanently - `F5` refresh.
Open tabs follow renames and moves.

### C/C++ and CMake

Open a folder containing a `CMakeLists.txt` and the IDE loads it like CLion does: it configures
`cmake-build-debug/` (Ninja if available, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`), reads the targets through
the CMake File API and shows a toolbar with the **profile** (Debug / Release / RelWithDebInfo), the
**target** (with *Program arguments...*), and Run / Build / Stop / Reload buttons. Output streams into the
bottom panel (**Build**, **Run**, **Problems**); compiler errors and warnings are clickable.
Saving `CMakeLists.txt` or a `.cmake` file reloads the project automatically.

`Ctrl+F9` build - `Shift+F10` build and run - `Ctrl+F2` stop - `Ctrl+Shift+R` reload CMake - ``Ctrl+` `` toggle panel.

Code intelligence comes from **clangd** (found on `$PATH`, or set `IDE_CLANGD=/path/to/clangd`); it is
pointed at the active profile's `compile_commands.json`. You get completion (as you type, or `Ctrl+Space`),
diagnostics (squiggles, gutter marks, counts in the status bar, hover for the message), hover documentation,
go to definition (`F12` or `Ctrl+click`, `Alt+Left` to go back) and header/source switching (`Alt+O`).


## Look: translucent chrome and blur

Daemon draws the window chrome (title bar, status bar, gaps between panels) translucent and asks the compositor to blur
what is behind it (`ext-background-effect-v1`, supported by GNOME 51+ and KWin). The panels themselves ("islands") are
opaque, and the code editor is near-black. Without alpha support or the protocol the chrome falls back to a solid colour;
set `DAEMON_OPAQUE=1` to force that.

## C++20 modules IntelliSense

Daemon starts clangd with `--experimental-modules-support` and `--completion-parse=always`. CMake + GCC projects that
use modules (`-fmodules-ts`, `import std;`) get a derived, Clang-flavoured compile database in
`<build>/.daemon/clangd/` (same sources, defines and includes, driven by `clang++` with libstdc++, duplicate BMI
entries folded away, libstdc++'s `bits/std.cc` kept as the `std` module source). clangd then builds its own BMIs, so
`import std;` and `import your.module;` resolve, and completion, diagnostics, hover and go-to-definition see through them.
The status bar shows `clangd · modules` when this is active.

On top of clangd: `import` statements complete project module names (and `std`, `std.compat`), and Go to Definition
on a module name jumps to its interface unit. `IDE_LSP_TRACE=1` prints the LSP traffic to stderr.
