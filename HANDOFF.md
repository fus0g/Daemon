# Daemon – handoff for a new agent

**What it is:** "Daemon", a Linux-only IDE (Wayland client + Vulkan 1.3 renderer, no toolkit) written in C++23 with C++20 modules (`import std`). Goal set by the user: feel like CLion / VS Code / Zed (editor, CMake projects, clangd intelligence, git, terminal, modern dark UI). Repo `/home/fus0g/Projects/ide`, ~15k lines, everything committed on the default branch (not a pushed remote; `git log` shows history). User works on Fedora, GNOME (Wayland, blur-capable), NVIDIA RTX 3050 Ti. Workflow: user asks for a feature/redesign, you build it, take a screenshot, user replies with visual feedback; commit after each finished change.

## 1. Working rules
- Build: `cmake --build build` (CMake 4.3, Ninja, GCC; ~1–3 min for app.cppm changes). Outputs `build/app/daemon` (IDE) and `build/tests/daemon_tests` (must print `all tests passed`). Grep build output with `grep -E " error|FAILED" -A5`.
- Never kill processes by name/pattern (a denied command once led to wrongly-applied patches). Use the scratchpad dir for temp files, not `/tmp`.
- Commit messages end with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.
- Pronouns for the user: avoid; use "you".
- App members live in `app/impl/*.cpp` (see source map); edit with the Edit tool or small Python replace scripts that assert each target occurs once.
- Report outcomes honestly; the user dislikes "AI-looking" UI – flat, restrained, consistent with existing islands.

## 2. Testing the UI without a display session
`./build/app/daemon --screenshot out.png --keys 'tok|tok|...' [folder]` renders frames offscreen-ish and writes a PNG (1280x800), then exits. Tokens (`App::stepScript`, app/impl/core.cpp):
- keys: `ctrl+x`, `ctrl+shift+n`, `enter`, `esc`, `tab`, `backspace`, `left/right/up/down`, `home/end`, `pgup/pgdn`, `f2 f3 f5 f9 f10 f12`; text: `t:hello` (types chars).
- `sleep:ms`, `wait`, `waitrun`; hooks: `side` (focus explorer), `filedrag` (fake a drag of 1st explorer row), `menu` (context menu), `gitsub:N`, `gitexpand:N`, `dragdemo:left|right|up|down|tabs`, `tooltip:N`, `blamehover`, `panelsel`, `hoverfake`, `welcome/browse/browseloc` variants (~l.255).
- Examples: welcome dialog `--keys 'ctrl+shift+p|sleep:600'`; new project `'ctrl+shift+p|ctrl+shift+n|sleep:600'`; terminal `'ctrl+`|sleep:1500|t:ls --color|enter|sleep:1500'`.
- Then Read the PNG to inspect. Real mouse interaction cannot be scripted, so pointer paths (drag/drop, selection) are only code-reviewed.
- Unit tests: `tests/tests.cpp` covers json, process/pty, LSP mock, cmake parsing, module scan, projects, git, terminal emulator.

## 3. Source map (modules are `daemon.<lib>.<name>`; C++ namespace `ide`)
Layers (dependencies point down): `app` -> `libs/services`, `libs/ui`, `libs/text` -> `libs/base`. Each module = `x.cppm` (interface: types, declarations, short inline functions) + `x.cpp` (implementation unit). `tools/` has the shader-compiler fallback, `third_party/` stb, Wayland XML, MDI font.
- `libs/base`: `util` (UTF-8, file I/O, PNG), `json`, `process` (pipes/PTY, `openPtyOnly`, `runCapture`).
- `libs/text`: `buffer` (lines + undo), `syntax`, `document` (cursor/selection/editing).
- `libs/ui` (reusable window + renderer framework, no IDE logic): `wayland`, `vulkan` (+ `shaders/`, protocols generated in its CMakeLists), `draw`, `font`, `icons`, `input`, `geometry`.
- `libs/services`: `lsp`, `cmake` (profiles, clangd DB derivation, module scan), `git`, `gdb` (MI3 client: `parseLine`, `Session`), `search`, `terminal` (VT emulator), `project`, `explorer` (+ `DirWatcher`, `fsops`).
- `app`: `app.cppm` = `class App` (all UI state + declarations, short inline members), `theme.cppm`, `options.cppm`, `cli.cpp` (`daemon_main`, called by the trivial `main.cpp`), and `impl/*.cpp` = App's member functions grouped by feature (activity_bar, build_run, core, debugger, dialogs, documents, editor_view, file_ops, git_view, keyboard, layout, lsp, pointer, prompts, render, search_view, terminal_view, ui_helpers). Adding a member function: declare it in `app.cppm`, define it in the matching `impl/` file (module implementation unit `module daemon.app;`).
- Tests: `tests/test_{base,text,ui,services}.cpp` + `test_main.cpp`, harness in `check.hpp`.
- GCC quirk: `app/main.cpp` must not `import daemon.app` (GCC 16 failed to read the app BMI from a TU using `std::println`); entry goes through `extern "C" daemon_main`. A defaulted friend `operator<=>` in a module ICEd, so `Pos` uses member operators.

## 4. UI structure (app/app.cppm + app/impl)
- `Layout L_`: `title`, `toolbar` (merged into the CSD title bar), `island` (editor area, a split tree of `Pane`s with tabs/breadcrumbs/minimap), `panel` (bottom island), `side`+`sideList` (explorer/git island), `activity` (right bar), `status`. Islands are rounded surfaces with small gaps over a blurred/darkened frame.
- **Right activity bar:** pill0 = Explorer(0), Git(1); pill1 = Output(2), Terminal(3), Problems(4). `activityActive/activityClick/activityRect/activityPill`. Icons are Material Design Icons (white, active = accent).
- **Bottom panel** (`panelTab_`): 0 Build, 1 Run (PTY stdin, `runFocus_`), 2 Problems (project-wide LSP/compiler problems, also badges in explorer), 3 Terminal. `drawPanel`, `panelClick`; header names array and loops are size 4; `panelTabRects_`/`panelScroll_`/`panelFollow_` arrays are size 4.
- **Terminal:** `startTerminal/pumpTerminal/openTerminal/termKey/drawTerminalBody/termClick`; spawns `$SHELL` in project root with `TERM=xterm-256color`, `COLORTERM=truecolor`; output pumped through `win_.watch(fd)`. Ctrl+` opens/toggles; Ctrl+Shift+C/V copy/paste; wheel scrolls scrollback (arrow keys in alt screen).
- **File watching:** `DirWatcher` (inotify, explorer.cppm) feeds `handleFsChanges` → explorer refresh, `checkDocsOnDisk` (reload unmodified docs), git refresh, search re-run.
- **Sidebar:** explorer (`drawSidebar`) or Git view (`sideGit_`). New file/folder/rename/delete use a card at the bottom of the sidebar (`sidePrompt()`, `drawSidePrompt`, prompt still driven by `prompt_`/`promptKey`). Drag to move: `fdrag_`, `fileDropTarget`, `finishFileDrag`; row click activates on mouse *release* (`dragRow_`). Other prompts (find/goto/open/save-as/run args) still use the bottom editor prompt bar (`drawPrompt`).
- **Tabs / panes:** drag tabs to reorder, move between panes or split (`tabDrag_`, `Zone`, `performDrop`); animated open/close.
- **Toolbar** (in title bar): profile & target chips, one outlined pill with Run/Debug(gdb)/Build/Regenerate/Stop (Stop red when a run is active).
- **Dialogs** (`DlgKind::Welcome|NewProject|Browse`, `drawDialog`): dim scrim + two islands drawn by `dlgSide`; clickable regions registered with `dlgHit(rect,id)` (ids `kDlg*`), helpers `dlgButton`, `dlgField`, `dlgCheckbox`, `welcomeAction`. Folder choosing goes through `browseOpen` → `zenity --file-selection --directory` run via `spawn()` (async job polled by `pollJobs`); `browseOpenBuiltin` is the fallback.
- **Theme** (`namespace theme`, app/theme.cppm): editorBg `0a0b0d`, sideBg `0d0e11`, sideSel `1a1c21`, sideHover `121317`, panelBg `0b0c0f`; accent = dark crimson (House Targaryen/Caraxes). Pills/outlines: white alpha ≈34 ring + `0a0b0d`/`0d0e11` fill, fully rounded. Material Design Icons via `Icons::glyph(dl_, font_, x, y, size, codepoint, color)`; codepoints in `icons.cppm` `mdi::` and inline in app.cppm (look up names at pictogrammers.com / the font's metadata).
- Gotchas: `Explorer::rows()` is non-const; `homeDir()` returns `std::string`; async work must `spawn` and mutate state only inside the returned lambda; clear LSP callbacks on destruction (`~App(){ lsp_.reset(); }`) to avoid heap corruption; draw activity bar *after* sidebar carve so buttons don't vanish during animation.

## 5. Language intelligence (hard-won, don't regress)
clangd runs with `--experimental-modules-support`. For CMake+GCC module projects the IDE derives a Clang-flavoured compile DB in `<build>/.daemon/clangd-v2` (adds `-std` from the compiler's `__cplusplus`, drops GCC-only flags, synthesizes `std` module source from libstdc++ `bits/std.cc`, folds duplicate BMI entries). Stale module caches crash clangd → auto wipe + restart (`handleLspCrash`). Hand-written module scanner (`scanModules`, no `std::regex`). Status bar shows `clangd · modules` when active. System deps: clangd, cmake, ninja, gcc/g++, gdb (Debug button), git, zenity; `IDE_CLANGD` overrides clangd path.

## 6. User's design preferences (learned by rejection)
Reverted: frosted-glass effect, translucent explorer/tab outlines, two-tone and red island outlines (invisible on white wallpaper), darker frame tint (user: "the background tint was good previously"). Liked: Material Design icons everywhere, white outlined pills, active icon in accent colour (not a tinted circle), outline pill for toolbar, plain white button icons, extra vertical padding inside pills, smaller activity buttons, red = warning-looking so UI text/accents use white where "red looks like an error". Editor background near-black. Dialogs should look like the rest of the IDE (islands, pills, explorer-style rows) – the user called earlier versions "AI crap".

## 7. State at handoff / not verified
Latest commits (newest first): handoff doc, system folder chooser (zenity), dialogs rebuilt from islands, welcome redesign, welcome visible during sidebar prompt, sidebar prompt card + explorer drag-move, sidebar icon set, integrated terminal.
Built and screenshotted but **not hand-tested with real input**: explorer drag/drop moves, terminal mouse selection/paste/vim/htop, rename/delete via sidebar card, the zenity flow (async, non-modal; shows "Choose a folder in the file dialog..." message), a user report that the welcome screen "disappears and comes back" when creating a file (fixed by keeping it visible during sidebar prompts; unconfirmed).
README.md is partially stale (e.g. "Look: translucent chrome and blur" section, key list, Output/Git panel wording); update it when touching those features.

## 8. Reasonable next steps
Clone-from-URL on the welcome screen; terminal tabs/splits and a real "Clear"; modal behaviour for the zenity dialog; move remaining editor-bar prompts (go to line, find) to a nicer in-editor overlay; hand-test the pointer features above and fix anything the user reports.
