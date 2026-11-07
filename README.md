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

## Editor panes: drag and drop

Drag a tab to reorder it within its strip, or drop it on another pane's strip or body to move it there. Dropping on the
left/right/top/bottom edge of a pane splits the editor and moves the tab into the new pane; the pane borders are
draggable. Esc cancels a drag, and a pane disappears when its last tab is closed or dragged out. Each file lives in
one pane at a time.

## Projects: create, open, recent

With no arguments Daemon starts on a welcome screen (also reachable by clicking the logo or Ctrl+Shift+P):

- **New Project** (Ctrl+Shift+N): pick a template (C++ executable, C++20 modules executable, static library, C executable,
  empty CMake project), a name, a location and a language standard, optionally with `git init`. Daemon writes a
  `CMakeLists.txt` and sources, opens the folder, configures CMake and opens the main file.
- **Open Folder** (Ctrl+Shift+O): an in-app folder browser with places, a path field, hidden-folder toggle and a
  "CMake" badge on folders that contain a `CMakeLists.txt`.
- **Recent projects** are kept in `$XDG_CONFIG_HOME/daemon/state.json` (default `~/.config/daemon/state.json`).

## Running programs

Programs started with Run (Shift+F10) run on a pseudo-terminal, so they behave as in a terminal: prompts appear
immediately (no output buffering), and `std::cin` / `scanf` read what you type. While a program runs, keystrokes in
the Run panel go to it (Esc or a click elsewhere releases them; Ctrl+C interrupts, Ctrl+D ends input). Click the Run
output to give the keyboard back to the program.

## Project-wide problems

clangd only reports diagnostics for files it has open, so Daemon quietly opens every source file of the project in the
language server (no tabs; a few per tick, up to 400 files). The Problems tab therefore lists errors and warnings from
the whole project, and the file explorer marks affected files (red / amber name and a count badge) and their folders.

## Git

Daemon talks to the `git` binary (no libgit2). In a repository you get:

- **Blame:** the cursor line shows who last changed it, when, and the commit summary (like Zed's inline blame, and it
  accounts for unsaved edits). Hover the annotation for the full commit card (author, date, message, hash) with a copy
  button; text in it is selectable.
- **Change markers** in the gutter against HEAD: green = added, blue = modified, a red wedge where lines were removed.
- **Explorer:** status letters (M modified, A added, U untracked, D deleted, R renamed, ! conflict) and a dot on folders
  that contain changes. The status bar shows the branch, ahead/behind counts and the number of changed files.
- **Git tab** (Ctrl+Shift+G, or the branch icon in the activity bar) with three views:
  - *Changes*: staged and unstaged files, per-file stage / unstage / discard (discard asks for a second click),
    Stage All / Unstage All, and a commit box (Enter commits).
  - *History*: the commit graph of all branches (lanes, merges, branch / tag chips, author, age, hash). Click a commit
    to expand its full message and changed files; click a file to open it.
  - *Branches*: local and remote branches with their last commit; Checkout switches branch.
  - Fetch / Pull / Push buttons (with ahead / behind counts) sit at the right of the tab header.

## Toolbar: run, debug, build, regenerate

The CMake toolbar has outlined selectors for the build profile and target, and one pill of actions: **Run** (
Shift+F10), **Debug** (Shift+F9), **Build** (Ctrl+F9), **Regenerate CMake** (Ctrl+Shift+R) and **Stop** (Ctrl+F2). Hovering a button shows its name and shortcut. Debug starts `gdb` on the
Run panel's terminal (it stops at `main`; type gdb commands there). Use the Debug profile so the program has symbols.
