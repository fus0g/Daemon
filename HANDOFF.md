# Daemon – handoff

Linux-only Wayland + Vulkan IDE, C++23 with C++20 modules (`import std`), CMake 4.3 + Ninja + GCC. Repo: `/home/fus0g/Projects/ide`, all work committed (latest: system folder chooser). User wants a CLion/VS-Code-like IDE; iterates on UI by screenshot feedback.

## Build / test / look
- `cmake --build build` → `build/daemon`, `build/ide_tests` (all pass). Don't kill processes by pattern.
- Screenshot: `./build/daemon --screenshot out.png --keys 'ctrl+shift+p|sleep:600' [folder]`. Key-script test hooks live in `App::stepScript` (`side`, `filedrag`, `menu`, `gitsub:N`, `dragdemo:`, `tooltip:N`, `waitrun`, `sleep:ms`, `t:text`, `ctrl+x`...). Use scratchpad dir for temp files.
- Commit each finished change; end commit messages with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`.

## Layout (src/)
`app.cppm` (~6.5k lines, everything UI: panes/tabs/splits, explorer, panel, toolbar, activity bar, dialogs, LSP glue, git UI, terminal UI) · `terminal.cppm` (VT emulator) · `process.cppm` (fork/PTY/env, `runCapture`) · `git.cppm` · `cmake.cppm` (profiles, clangd derived compile DB, module scan, CMake completion) · `lsp.cppm` · `project.cppm` (templates, state.json recents) · `explorer.cppm` (tree + `fsops`) · `draw/vulkan/wayland/font/icons.cppm`, `shaders/`. Tests: `tests/tests.cpp`.

## Key facts
- Icons: Material Design Icons font (`third_party/fonts/materialdesignicons.ttf`), codepoints via `Icons::glyph`; name→codepoint table was at scratchpad `mdi.json`.
- Theme in `app.cppm` `theme::` (editorBg 0a0b0d, sideBg 0d0e11, accent = Targaryen crimson). User rejected: frosted glass, translucent explorer/tabs, two-tone/red outlines, darker tint – don't reintroduce. Pills = white outline (alpha ~34) + dark fill.
- Dialogs (`DlgKind` Welcome/NewProject/Browse) are drawn from two islands via `dlgSide`; hits via `dlgHit` ids.
- Open Folder / Browse… use `zenity --file-selection --directory` async (`spawn`/`pollJobs`), built-in browser (`browseOpenBuiltin`) only as fallback.
- Bottom panel tabs: Build, Run, Problems, Terminal (index 3, `panelTab_`); Ctrl+` opens the terminal. Activity bar (right): pill0 Explorer+Git, pill1 Output+Terminal+Problems.
- New file/folder/rename/delete prompt is a card at the sidebar bottom (`sidePrompt()`); explorer rows move by drag (`fdrag_`), open/toggle on mouse release.
- clangd: needs `--experimental-modules-support`, derived DB in `.daemon/clangd-v2`, crash-recovery wipes module cache. System deps: clangd, cmake, ninja, gcc, gdb (debug), git, zenity.
- Heap-corruption lesson: LSP callbacks cleared in `~LspClient`, `~App(){ lsp_.reset(); }`.

## Untested / watch
- Real mouse drag-and-drop in explorer, terminal selection/paste/vim, rename/delete via the sidebar card, zenity chooser flow – only built/screenshotted, not hand-tested.
- User reported welcome screen flicker when creating a file; fixed by keeping it visible during sidebar prompts, unconfirmed.

## Ideas not done
Clone-from-URL on welcome screen, terminal tabs/splits, terminal "Clear" semantics, README mentions only the main features.
