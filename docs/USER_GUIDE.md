# Daemon user guide

Detailed reference for every feature. For a short overview see the [README](../README.md).

## Keys

`Ctrl+O/N/S/Shift+S/W/Q` open/new/save/save as/close tab/quit - `Ctrl+Shift+O` open folder, `Ctrl+B` toggle sidebar, `Ctrl+Shift+E` focus it (arrows, Enter, `F5` refresh; drag the gap to resize) - `Ctrl+F/H/G` find/replace/go to line (`F3` next) -
`Ctrl+Z/Y` undo/redo - `Ctrl+/` comment - `Ctrl+Shift+D/K` duplicate/delete line - `Alt+Up/Down` move line -
`Ctrl+Shift+V` Markdown preview - `Ctrl+]/[` indent - `Ctrl+Tab`, `Ctrl+PgUp/PgDn` switch tab - `Ctrl +/-/0` or `Ctrl+wheel` zoom.

### Markdown preview

Open a `.md` file and press `Ctrl+Shift+V` (or right-click > *Open Markdown Preview*): a read-only **Preview** tab opens to the
right and follows the source live as you type. Press the shortcut again (or close the tab) to remove it. Click a link to open it:
web links go to your browser, `#section` links scroll to the heading, relative links open the file in the editor. Scroll with the
wheel, arrow keys, `PgUp/PgDn`, `Home/End` or `Space`.

Rendered: headings, paragraphs, bold, italic, strikethrough, inline code, links, block quotes, bullet / numbered / task lists
(nested), fenced code blocks with syntax colours, tables (with column alignment, wrapping to the pane width), horizontal rules and
the common HTML in READMEs (`<br>`, `<img>`, `<a>`, `<kbd>`; centred blocks are shown left-aligned).

**Images** are drawn for real: PNG, JPEG, GIF (first frame), BMP and SVG, from files next to the document (relative paths, or `/path`
relative to the project) and from `https://` URLs (badges, screenshots). Remote pictures are downloaded in the background with `curl`
and cached in `~/.cache/daemon/images` (pictures unused for 30 days are removed); set `DAEMON_NO_REMOTE_IMAGES=1` to never fetch
anything. Plain `http://` and `data:` sources are not loaded. `<img width= height=>` sizes are honoured, wide pictures shrink to
the pane, small ones (badges) sit inline with the text, and a picture that cannot be loaded shows its alt text as a chip. SVG
support is NanoSVG's (shapes, paths, gradients; no CSS, filters or clip paths) plus our own `<text>` drawing, which is what shields.io
badges need. The preview tab is part of the saved session.

### Notebooks (C++ in `.ipynb` files)

Open or create a file ending in `.dcxxnb` (Daemon's own extension) or `.ipynb` (File > Open, or a new file with that name) and it opens as a notebook: a list of
**code** and **markdown** cells, each with its own editor. Nothing else has to be installed: Jupyter is not used, the notebook
runs on Daemon's own C++ kernel, and both extensions hold the same ordinary Jupyter notebook JSON (nbformat 4): rename a `.dcxxnb` to `.ipynb` and other tools can open it.

**Running.** `Shift+Enter` runs the cell and moves on (adding a cell after the last one), `Ctrl+Enter` runs it in place,
`Alt+Enter` runs it and inserts a cell below. The toolbar has *Run*, *Run all* (`Ctrl+Shift+Enter`), *Stop*, *Restart*, *+ Code*,
*+ Text*, *Delete* and *Save* (`Ctrl+S`). Output, the value of a last expression written without a semicolon (`v.size()`), and
compiler errors with `cell:line:col` positions appear under the cell. Variables, functions, classes, templates and `using`
declarations stay alive between cells and between runs of a cell, until you press *Restart*.

**Modes** (as in Jupyter). `Esc` leaves editing: arrow keys / `j` `k` move between cells, `Enter` edits, `a` / `b` insert a cell
above / below, `dd` deletes, `m` / `y` turn a cell into markdown / code. Markdown cells are rendered like the preview (double
click or `Enter` edits). `Up` / `Down` at the first / last line of a cell move into the neighbouring one.

**How it works.** Every cell is compiled with GCC as a C++20 module and loaded into a long-lived runner process, so the
compiler itself carries types and `auto` variables from cell to cell. A cell is split into declarations (exported, kept) and
statements (run once); `#include`s, macros, `using namespace` and `import`s are repeated in later cells automatically. That
makes `import std;` work, and so do modules of your own: a cell that starts with `export module geometry;` is compiled as that
module, and a later cell can `import geometry;`.

**IntelliSense.** Cells have completion (as you type, after `.` / `->` / `::`, or with `Ctrl+Space`; `Tab` / `Enter` accept),
error squiggles and hover information, from clangd (`clangd` on `PATH`, or `IDE_CLANGD`). The server sees all code cells as one C++
file (statements wrapped in functions, `import std;` read as the standard headers, module cells inlined), so a name from an earlier
cell completes in a later one. It is a preview of what the compiler will say, not a substitute: a cell can still fail to
compile when it runs. Errors from a failed run are shortened to the errors themselves, with the first few notes.

**Magics.** Lines at the top of a cell: `%flags -O2 -I/some/include -DFOO` adds compiler flags for this and later cells,
`%link -lm -lfoo` adds libraries.

**Limits.** Needs **GCC 15 or newer** (`g++` on `PATH`, or set `DAEMON_NB_CXX`); the first run builds the `std` module (about
5 s, then cached in `~/.cache/daemon/notebook`), a cell takes about a second to compile. Redefining a name that an earlier
cell defined is an error unless you re-run that same cell. A module cell (`export module x;`) can be built once per kernel:
change it and press *Restart* (or rename it). A top-level variable declared in a cell is a global, so it keeps its
value; structured bindings (`auto [a, b] = ...`) are local to their cell. *Stop* kills the kernel (a runaway loop cannot be
interrupted otherwise), which also forgets the variables. `std::cin` reads nothing. Plots and images are not shown yet. Notebooks
of other languages open for reading and editing but cannot be run.

`daemon --nb-run in.dcxxnb [out.dcxxnb]` runs all cells without a window and writes the outputs (handy for CI).

### Structure view
Below the file tree the sidebar lists what is in the current file: namespaces, classes, structs, enums, functions, methods, fields
and constants of C/C++ files (it asks clangd, so it appears once clangd has read the file; the first time can take a moment in
large module projects) and the headings of Markdown files. Icons show the kind, grey text the type or signature. Click a row to jump
to the symbol, click the arrow to fold it; the symbol the cursor is in is highlighted and scrolled into view. Click the STRUCTURE
header to collapse the view, drag the divider above it to resize. Turn it off or set its height in Settings > Appearance.

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


## Settings
Open them with **Ctrl+,** or File > Settings... A search box at the top finds a setting by name, description or key; the sections on
the left group them. Changes apply immediately and are saved to `~/.config/daemon/settings.json` (`$XDG_CONFIG_HOME/daemon/`).
Only values that differ from the default are stored, as flat `"section.name"` keys:
```json
{
  "editor.fontSize": 16,
  "editor.tabWidth": 2,
  "lsp.clangd": "/opt/llvm/bin/clangd"
}
```
The file can also be edited by hand ("Open settings.json" in the dialog); Daemon notices the change within a couple of seconds.
A value of the wrong type or outside the allowed range falls back to the default and the dialog says which key it was. Unknown
keys are left alone. A coloured label means the value is not the default; the arrow next to it resets just that setting.

| Section | Settings |
|---|---|
| Editor | font size, tab width, indent with spaces, detect indentation, close brackets and quotes, minimap, breadcrumbs, blinking cursor, reload changed files |
| Appearance | interface font size, animations, start maximized (restart) |
| General | reopen the last project, restore open tabs |
| Git | change markers, inline blame |
| C/C++ intelligence | use clangd, clangd program, background indexing |
| Terminal | shell |
| Notebooks | compiler, extra compiler flags |
| Markdown | load images from the web |
| Tools | the toolchain check (what is installed, Download / install command / Re-check) and the reminder at start-up; also Help > Check Toolchain... |

Environment variables still win over the file where they existed before: `IDE_CLANGD`, `IDE_BGINDEX=0`, `DAEMON_NB_CXX`,
`DAEMON_NB_FLAGS`, `DAEMON_NO_REMOTE_IMAGES`, and `--font-size` on the command line. Zooming with Ctrl+= / Ctrl+- changes
`editor.fontSize`.

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

### Sessions

Daemon remembers, per project, which files are open, how the editor is split, which tab is active and where the cursor and
scroll position were. Opening the project again (welcome screen, recent list, `daemon <folder>`) restores it, and starting
`daemon` without arguments reopens the last project. The session is written a few seconds after every change and on exit, to
`~/.local/state/daemon/sessions/`. Untitled buffers are not restored and unsaved edits in open tabs are never replaced; files
that no longer exist are skipped. `DAEMON_NO_SESSION=1` turns the feature off; `daemon file1 file2` opens exactly those files.

## Running programs

Programs started with Run (Shift+F10) run on a pseudo-terminal, so they behave as in a terminal: prompts appear
immediately (no output buffering), and `std::cin` / `scanf` read what you type. While a program runs, keystrokes in
the Run panel go to it (Esc or a click elsewhere releases them; Ctrl+C interrupts, Ctrl+D ends input). Click the Run
output to give the keyboard back to the program.

## Project-wide problems

The Problems tab has a **Copy all** button (plus **Copy errors** when there are warnings too) that copies every entry as
`file:line:col: error: message`, one per line, ready to paste into a chat or an issue.

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
- **Source Control** (Ctrl+Shift+G, or the branch icon next to the Explorer button; it replaces the file tree in the right-hand island) with three views:
  - *Changes*: staged and unstaged files, per-file stage / unstage / discard (discard asks for a second click),
    Stage All / Unstage All, and a commit box (Enter commits).
  - *History*: the commit graph of all branches (lanes, merges, branch / tag chips, author, age, hash). Click a commit
    to expand its full message and changed files; click a file to open it.
  - *Branches*: local and remote branches with their last commit; Checkout switches branch.
  - Fetch / Pull / Push buttons sit at the right of the header (icons only when the island is narrow).

## Toolbar: run, debug, build, regenerate

The CMake toolbar has outlined selectors for the build profile and target, and one pill of actions: **Run** (
Shift+F10), **Debug** (Shift+F9), **Build** (Ctrl+F9), **Regenerate CMake** (Ctrl+Shift+R) and **Stop** (Ctrl+F2). Hovering a button shows its name and shortcut. Debug starts `gdb` on the
Run panel's terminal (it stops at `main`; type gdb commands there). Use the Debug profile so the program has symbols.

## Integrated terminal

The bottom panel has a **Terminal** tab (Ctrl+` or the terminal button in the activity bar). It runs your `$SHELL` in a real PTY
in the project folder, with an xterm-256color / truecolor emulator (scrollback, alternate screen, bracketed paste, mouse-wheel
scrolling). Select with the mouse; copy with Ctrl+Shift+C (or Ctrl+C while text is selected), paste with Ctrl+Shift+V.

## Explorer

New file / new folder / rename / delete are asked for in a card at the bottom of the sidebar. Files and folders can be dragged
onto a folder in the tree (or the empty area for the project root) to move them; open tabs follow the moved files.

## Search and replace in files

`Ctrl+Shift+F` (or the magnifier in the activity bar) opens project-wide search in the sidebar; `Ctrl+Shift+H` focuses the replace field.
Toggles: match case (`Aa`, Alt+C), whole word (`ab`, Alt+W), regular expression (`.*`, Alt+R; `$1` works in the replacement), and
**Filters** for *files to include / exclude* globs (comma separated, e.g. `*.cpp, src/**`). Files are taken from git (`.gitignore` is
respected) or, outside a repository, from a walk that skips build and vendor folders; binary and >2 MB files are skipped. Unsaved
editor contents are searched too. Results are grouped per file; click one to jump to it, hover for *replace* / *dismiss*, use the
button in the replace field for *Replace All* (asks for a second click). Files with unsaved edits are never overwritten.

## Debugger

Debugging runs your program under **gdb** through its machine interface (MI3). Start with the bug button in the toolbar, `Shift+F9`
or `F5` (`F10` / `F11` start and stop at `main`). The program's own input/output lives in the **Run** tab; the **Debug** tab is a
console where you can evaluate expressions (`x + 1`, `*ptr`) or run any gdb command (`bt`, `info registers`, `disassemble`).

- **Breakpoints:** click the gutter or `F9`; right-click for conditional breakpoints and logpoints (`log: x = {x}` prints and
  continues). Edit text accepts a condition, `#N` to skip the first N hits, or `log: message`. Breakpoints can be disabled,
  function breakpoints and *break on C++ throw* are in the BREAKPOINTS section, and *data breakpoints* (stop when a value changes) are
  one click on a variable. Breakpoints are saved per project in `.daemon/debug.json` and can be changed while the program runs.
- **Stepping:** `F5` continue, `F6` pause, `F10` over, `F11` into, `Shift+F11` out, `Ctrl+F10` run to cursor, `Ctrl+Shift+F10` set next
  statement, `Shift+F5` stop, `Ctrl+Shift+F5` restart (also the floating bar over the editor).
- **Run and Debug view** (sidebar): VARIABLES (expandable, STL containers via gdb's pretty printers, edit a value, copy, add to watch,
  watch for changes), WATCH expressions, CALL STACK with threads (click a frame to inspect it) and BREAKPOINTS. Hover an expression in
  the editor while paused to see its value; changed values are highlighted.

