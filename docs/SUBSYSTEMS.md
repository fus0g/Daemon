# Subsystems

## Language intelligence (clangd + CMake) — `services/lsp`, `services/cmake`, `app/impl/lsp.cpp`, `build_run.cpp`
- Opening a folder with `CMakeLists.txt` configures `cmake-build-<profile>/` (Ninja, `CMAKE_EXPORT_COMPILE_COMMANDS=ON`), reads targets
  through the CMake File API, and shows profile/target chips and Run/Debug/Build/Regenerate/Stop in the title-bar toolbar.
- clangd is started with `--experimental-modules-support --completion-parse=always` on a **derived compile database**
  (`<build>/.daemon/clangd-v2/`, `prepareClangdDb`): Clang-flavoured flags for GCC projects, `-std` derived from the compiler's
  `__cplusplus` (CMake omits the default), GCC-only flags dropped, libstdc++'s `bits/std.cc` used as the `std` module source,
  duplicate BMI entries folded, module sources synthesised. This is what makes `import std;` / `import your.module;` work.
- A hand-written module scanner (`scanModules`, `importNameStart`) feeds `import` completion and go-to-module. Stale module caches make
  clangd crash: `handleLspCrash` wipes the cache and restarts. Status bar shows `clangd · modules` when active.
- Diagnostics from clangd are collected project-wide (`diags_`), merged with compiler output into the **Problems** tab and explorer badges.
- CMakeLists.txt completion: `CMakeKnowledge` + `cmakeCompletions`.

## Git — `services/git`, `app/impl/git_view.cpp`
Shells out to `git` (`--no-optional-locks`). Status (`--porcelain -z`) polled every 3 s while focused and after file changes;
gutter change bars come from an in-process Myers diff against `HEAD`; inline blame (`--porcelain --contents`) with a commit card on hover;
Git sidebar view with Changes (stage/unstage/discard/commit), History (graph lanes + expandable commit details) and Branches
(checkout, fetch/pull/push). Explorer shows status letters and folder dots.

## Debugger — `services/gdb`, `app/impl/debugger.cpp`
- Protocol: gdb MI3 over pipes (`gdb --interpreter=mi3 -q -nx`). `gdb::Session::send(cmd, callback)` tokens commands; `onAsync` gets
  `*stopped/*running/=breakpoint-modified`; `onStream` gets console text. `parseLine` converts MI records to `json::Value`
  (tuples -> objects, lists -> arrays).
- Inferior I/O: `Process::openPtyOnly()` creates a PTY; its slave path is passed via `-gdb-set inferior-tty`; the master is pumped into the
  **Run** tab (so program input/output works there).
- Session life: `debugLaunch` (setup commands, insert breakpoints, `-exec-run [--start]`), `dbgAsync` -> `dbgOnStopped` (reason handling,
  logpoints auto-continue, watchpoint text, exit detection), `dbgEnd` (kills gdb, resets state; never call it from inside a gdb callback —
  use `dbgScheduleEnd`, which `pumpDebug` executes after `pump()` returns).
- State: `dbgState_` (Off/Starting/Running/Stopped), `dbgGen_` (bumped on every run/stop to drop stale callbacks), `dbgVarGen_` (variable
  tree generation), `bps_` (persisted in `.daemon/debug.json`), `watches_`, `dbgFrames_`, `dbgThreads_`, `dbgLocals_`/`dbgWatchVars_`
  (varobj trees; children fetched lazily; C++ `public/private` pseudo-nodes are flattened; pretty-printed containers are `dynamic`).
- Breakpoints: line, function, data (`-break-watch`), C++ `throw` (`-catch-throw`), conditions (`-c`), skip counts (`-i`), logpoints
  (implemented in the IDE: break, evaluate `{expr}`, print to the console, continue). Changes while running use `dbgWhenPaused`
  (interrupt, apply, continue).
- UI: floating control bar (`drawDebugBar`), gutter dots + execution arrow/line (`drawDebugGutter`), hover value tooltip
  (`App::hoverTick` in `impl/lsp.cpp`), Run and Debug sidebar (`drawDebugBody`: rows from `dbgBuildRows`, geometry `dbgLayoutRows`), Debug console
  panel tab (`drawDebugConsole`, evaluation queue `dbgEvalNext`: `-data-evaluate-expression`, fallback to `-interpreter-exec console`).
- Keys: F5 continue/start, Shift+F5 stop, Ctrl+Shift+F5 restart, F6 pause, F9 breakpoint, F10/F11/Shift+F11 step, Ctrl+F10 run to
  cursor, Ctrl+Shift+F10 set next statement.

## Project search & replace — `services/search`, `app/impl/search_view.cpp`
`search::run` lists files (`git ls-files -z --cached --others --exclude-standard` or a directory walk skipping build/vendor dirs), skips
binary and >2 MB files, applies include/exclude globs, matches line by line (`Matcher`: literal or ECMAScript regex, case, whole word),
and returns grouped matches with trimmed previews. Unsaved editor buffers are searched via `overrides`. The UI debounces typing
(`scheduleSearch`, 280 ms), runs the search in a `spawn` job guarded by a generation counter, and replaces by rewriting files on disk
(`replaceInFile`); files with unsaved edits are skipped; open unmodified files reload through the file watcher path.

## Integrated terminal — `services/terminal`, `app/impl/terminal_view.cpp`
`Terminal` is a VT100/xterm emulator (SGR colours incl. 256/truecolor, cursor movement, scroll regions, alternate screen, scrollback
5000, bracketed paste, DSR replies via `takeReply()`). The shell (`$SHELL`) runs on a PTY in the project root; `termKey` encodes keys,
`termClick`/`drawTerminalBody` handle selection and drawing. Ctrl+` toggles; Ctrl+Shift+C/V copy/paste.

## File watching — `services/explorer` (`DirWatcher`), `app/impl/documents.cpp`
inotify watches every directory the explorer has loaded plus the parent directory of every open file. Events are debounced (150 ms)
into `handleFsChanges`: explorer refresh, `checkDocsOnDisk` (reload unmodified documents keeping cursor/scroll; warn when a document
with unsaved edits changed or a file was deleted), git refresh, search re-run. `Document::diskTime` prevents reloading our own saves.

## Projects and dialogs — `services/project`, `app/impl/dialogs.cpp`
Templates (C++ executable, modules executable, static library + example, C executable, empty CMake) via `createProject`; recents in
`~/.config/daemon/state.json`. Dialogs (Welcome, New Project, folder browser) are modal overlays built from two islands (`dlgSide`)
with hit regions registered by `dlgHit(rect, id)`. "Open folder" and "Browse..." use `zenity --file-selection --directory` through
`spawn`; the built-in browser is the fallback.

## Explorer and file operations — `services/explorer` (`fsops`), `app/impl/file_ops.cpp`
Tree with lazy loading; create/rename/delete/duplicate/copy/cut/paste (trash follows the freedesktop spec), drag to move (`fdrag_`),
context menu. New file/folder/rename/delete prompts appear as a card at the bottom of the sidebar (`sidePrompt()`).

## Toolchain check (libs/services/toolchain, app/impl/setup.cpp)
At start Daemon looks for clangd, cmake, ninja, gdb, git (recommended) and clang++, zenity (optional), reads their versions
(`--version`) and flags ones that are missing or older than the minimum (clangd 16, cmake 3.20, gdb 10, git 2.20). If something is
wrong and the user has not pressed "Don't remind me" (`state.json: setupMuted`), the Setup dialog opens: per-tool status, the
install command for the detected distribution (`/etc/os-release` -> dnf/apt/pacman/zypper/apk package names), **Run in terminal**
(types it into the integrated terminal so the user enters their sudo password there), **Copy command**, **Re-check**. Always
reachable from the welcome screen ("Check Tools"). Test hook: `--keys setup`.

Shipped or managed tools take priority: `$DAEMON_TOOLS_DIR/bin`, `<exe>/../libexec/daemon/bin`, `<exe>/tools/bin`,
`~/.local/share/daemon/tools/bin`, then `$PATH`; `toolchain::init()` prepends these to `$PATH` so child processes use them too.
Dropping `clangd`, `cmake` and `ninja` binaries there is all a package needs to do to bundle them.

### Managed downloads
For clangd, CMake and Ninja the Setup dialog offers **Download** when the tool is missing or too old. `toolchain::install` (worker thread):
GitHub `releases/latest` JSON -> pick the Linux asset for the CPU (`pickAsset`) -> download with `curl` (or `wget`) -> verify the
SHA-256 digest GitHub publishes for the asset (refuses to install without one) -> extract (`tar`, or `unzip` / `bsdtar` / `python3 -m zipfile`)
into `~/.local/share/daemon/tools/<tool>/<version>/` -> symlink the executables into `tools/bin` (first on PATH) -> run `--version`.
Progress is the size of the growing file. gdb, git, zenity and clang have no official binaries: package manager only.
Real-network test: `DAEMON_TEST_NET=1 ./build/tests/daemon_tests`. Test hook: `--keys 'setup|toolget:ninja'`.
