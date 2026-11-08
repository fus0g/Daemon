# Architecture

## Layers
```
app            daemon_app        the IDE: class App, theme, CLI           depends on everything below
libs/services  daemon_services   LSP, CMake, git, gdb, search, terminal emulator, projects, file tree
libs/ui        daemon_ui         Wayland window + Vulkan renderer + fonts/icons/drawing    (generic, no IDE logic)
libs/text      daemon_text       text buffer, syntax highlighting, document (cursor/selection/editing)
libs/base      daemon_base       UTF-8/file helpers, JSON, child processes / PTYs
```
Dependencies only point downwards (`services` and `ui` depend on `base`; `text` on `base`; `app` on all). Each library is a CMake
static library whose `.cppm` files are a `FILE_SET CXX_MODULES` and whose `.cpp` files are module implementation units.

### Module list (`libs/<lib>/<name>.cppm` + `.cpp`)
| module | contents |
|---|---|
| `daemon.base.util` | UTF-8 decode/encode, `readFile/writeFile`, `trim`, `homeDir`, `expandPath`, PNG writer |
| `daemon.base.json` | `json::Value` (insertion-ordered objects), parser, `dump` |
| `daemon.base.process` | `Process` (pipes or PTY, `start`, `read`, `write`, `terminate`, `tryWait`, `openPtyOnly`, `runCapture`), `findExecutable` |
| `daemon.text.buffer` | `Buffer` (lines, grouped/coalescing undo-redo), `Pos`, `Selection` |
| `daemon.text.syntax` | `Highlighter`, `Lang`, token kinds (`Tok`) for C/C++, Python, Rust, JS/TS, Go, ... |
| `daemon.text.document` | `Document`: path, buffer, cursor/selection, editing commands, find, bracket matching, disk-stamp for reload |
| `daemon.ui.wayland` | `Window`: xdg-shell toplevel, client-side decorations, xkbcommon keys, clipboard, cursors, HiDPI scale, background blur, `watch(fd, cb)` poll hooks |
| `daemon.ui.vulkan` | `Renderer`: swapchain, instanced-quad pipeline (+ glass blur pass), atlas upload, screenshot readback |
| `daemon.ui.draw` | `Color`, `Instance`, `DrawList` (per-frame quad list; modes Normal / Erase / Glass; rounded rects, rings, text) |
| `daemon.ui.font` | system font discovery, stb_truetype rasteriser, glyph atlas (code font, UI font, title font, MDI icon font) |
| `daemon.ui.icons` | `Icons` (Material Design glyphs, file/folder icons, chevrons, tool icons) and the `mdi::` code points |
| `daemon.ui.input` | `Key`, `KeyEvent`, `Button`, `Cursor`, `Edge` enums |
| `daemon.ui.geometry` | `Rect` |
| `daemon.services.lsp` | `LspClient` (JSON-RPC over pipes; completion, hover, definition, diagnostics, cancel) |
| `daemon.services.cmake` | profiles, File-API targets, build/run argv, error parsing, clangd DB derivation, module scanner, CMakeLists completion |
| `daemon.services.git` | shell-out git API: status, diff (Myers, in-process), blame, log graph, branches, stage/commit/pull/push |
| `daemon.services.gdb` | gdb/MI3: `parseLine` (records -> json), `Session` (async commands with callbacks), `quote` |
| `daemon.services.search` | globs, `Matcher` (literal/regex/case/word), file listing (git or walk), `run`, `replaceInText/File` |
| `daemon.services.terminal` | VT/xterm emulator (cells, scrollback, alt screen, colours, bracketed paste) |
| `daemon.services.project` | project templates (`createProject`), recent projects (`DaemonState`, `~/.config/daemon/state.json`) |
| `daemon.services.explorer` | `Explorer` file-tree model, `fsops` (create/rename/trash/copy/move), `DirWatcher` (inotify) |
| `daemon.app.theme` / `daemon.app.options` | colours (`ide::theme::*`), command-line options |
| `daemon.app` | `class App` |

## Process structure
`main.cpp` -> `daemon_main` (`app/cli.cpp`: parse options, build `App`, `init()`, `run()`).

### Frame loop (`App::run`, `app/impl/core.cpp`)
```
loop: compute poll timeout (0 if dirty/animating, else until nextWake())
      win_.pump(timeout)   -> Wayland events + any fd registered with win_.watch(fd, cb) (child processes, inotify, gdb ...)
      update(now, dt)      -> timers, animations, debounced work (search, fs changes, git poll, LSP flush, jobs)
      if dirty_: draw()    -> builds the DrawList (layout(), then panes, sidebar, panel, overlays)
                 ren_.render(dl_, font_, frameC())   -> Vulkan present
```
Everything is immediate-mode: `draw()` re-emits all quads each frame from `App` state; there are no widget objects. Input handlers
mutate state and set `dirty_`. Hit-testing re-computes the same rectangles (helpers such as `activityRect`, `dbgRowAt`, or hit lists
recorded while drawing: `gitHits_`, `sHits_`, `dlgHits_`, `dbgBar_`).

### Threads and async
Single UI thread. `spawn(work)` (std::async) runs blocking work (git, search, zenity, CMake File API) and returns a lambda that
`pollJobs()` applies on the UI thread. Subprocess output is non-blocking and pumped from `win_.watch(fd)` callbacks
(`pumpRun`, `pumpTerminal`, `pumpDebug`, LSP, build output, inotify).

### Rendering model
- One pipeline draws instanced quads (rects, rounded rects via SDF, rings, glyphs from an R8 atlas).
- Window chrome (title/status/gaps) is translucent and blurred by the compositor (`ext-background-effect-v1`); islands are opaque.
  `carve(rect)` punches the frame colour back into a region ("Erase" mode) so scrolled content cannot overflow its island.
- `glassPanel()` draws menus/dialog-like surfaces using the Glass mode (scene copy blur). Screenshots render without the compositor
  blur (frame is opaque).
- Text: monospace code font (`font_`), proportional UI font helpers `uiLabel/uiLabelR/uiLabelC/uiW/uiH`, title font for headings.

## `App` structure (`app/app.cppm` + `app/impl/*.cpp`)
`App` owns all UI state in one class. `app.cppm` lists declarations grouped by comment banners (`// ==== section`), the same order as
the implementation files. Implementation files and their sections:

| file | responsibility |
|---|---|
| `core.cpp` | init, `run`, `update`, test-script interpreter (`stepScript`), messages, zoom, title, screenshots, file-watch glue |
| `documents.cpp` | documents/tabs, open/close/save, folder open, inotify handling, reload on disk change |
| `layout.cpp` | `layout()` -> `Layout L_`, animations, pane split tree (`Pane`, `Node`, `Zone`), tab strips |
| `render.cpp` | `draw()`, editor drawing (text, gutter, selections, diagnostics, blame, minimap), status bar, overlays |
| `editor_view.cpp` | minimap |
| `ui_helpers.cpp` | UI font helpers |
| `activity_bar.cpp` | right-edge pills (Explorer/Search/Git, Output/Terminal/Problems, Debug) |
| `keyboard.cpp` / `prompts.cpp` | global key routing, editor shortcuts, prompts (find/goto/open/save-as/rename...), sidebar prompt card |
| `pointer.cpp` | mouse buttons/move/scroll, tab drag & drop, gutter clicks, context menus |
| `lsp.cpp` | clangd glue: start/crash recovery, completion, hover, diagnostics, navigation, module list |
| `build_run.cpp` | CMake configure/build/run, toolbar, output panel (Build/Run/Problems), run input PTY |
| `file_ops.cpp` | explorer operations, context menu, drag-to-move, file clipboard |
| `git_view.cpp` | status/gutter diff/blame, Git sidebar view (changes/history/branches) |
| `terminal_view.cpp` | integrated terminal panel |
| `search_view.cpp` | project search & replace view |
| `debugger.cpp` | gdb session, breakpoints, variables, console, Run and Debug view, floating bar, gutter markers |
| `dialogs.cpp` | welcome / new project / folder browser dialogs, text boxes |

Key state groups (all members of `App`): documents `docs_`/`cur_`; panes `root_`/`activePane_`; layout `L_`; sidebar flags
`sidebar_`, `sideGit_`, `sideSearch_`, `sideDebug_`, `sideFocus_`; bottom panel `panelOpen_`, `panelTab_` (0 Build, 1 Run, 2 Problems,
3 Terminal, 4 Debug); prompt `prompt_`; menu `menu_`; dialogs `dlg_`; explorer `explorer_`; LSP `lsp_`; CMake `profiles_/targets_`;
run `runProc_/runOut_`; terminal `term_/termProc_`; git `gitRoot_/gitEntries_/...`; search `sQuery_/sRes_/...`; debugger `dbg_/bps_/dbgLocals_/...`.

## Persistence
- `~/.config/daemon/state.json`: recent projects, last project location.
- `<project>/.daemon/debug.json`: breakpoints and watch expressions. `<build>/.daemon/clangd-v2/`: derived compile database for clangd.
