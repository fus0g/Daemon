# Feature inventory (what exists today)

**Editor**: tabs with animated open/close and drag & drop (reorder, move between panes, split), split panes, breadcrumbs, minimap,
line numbers, animated stretchy caret, syntax highlighting, bracket matching, find/replace in file (Ctrl+F / Ctrl+H, F3), go to line
(Ctrl+G), undo/redo, comment toggle (Ctrl+/), line duplicate/delete/move, indent/outdent, zoom (Ctrl +/-/0, Ctrl+wheel), selection and
clipboard, automatic indentation detection, UTF-8, CRLF preserved, files reload when changed on disk.

**C/C++ intelligence (clangd)**: completion with kind icons, diagnostics (squiggles, gutter, counts), hover docs, go to definition
(F12 / Ctrl+click, Alt+Left back), header/source switch (Alt+O), C++20 modules (`import std;`, project modules), project-wide problems list.
**CMake**: configure/build/run/regenerate from the toolbar, profiles and targets, program arguments, clickable compiler errors,
CMakeLists.txt completion.

**Explorer** (sidebar): file icons (Material Design), git status letters, problem badges, context menu, copy/cut/paste/duplicate,
rename, trash, drag to move, live refresh on external changes. **Projects**: welcome screen, New Project wizard, recent projects,
system folder chooser.

**Search** (Ctrl+Shift+F / Ctrl+Shift+H): project-wide find and replace with regex, case, whole word, include/exclude globs.

**Git**: gutter changes, inline blame, Changes/History/Branches views, stage/commit/pull/push/checkout.

**Run & Debug**: run with stdin (Run tab on a PTY); gdb debugger with breakpoints (conditional, logpoints, data, function, C++ throw),
step/continue/pause/run to cursor/set next statement, call stack and threads, variables and watches with editing and
expansion, hover values, Debug console, floating control bar.

**Integrated terminal** (Ctrl+`): real PTY shell with xterm emulation, selection and copy/paste.

**Window**: native Wayland with client-side decorations, merged title/toolbar, the logo as a round pill at the left (slides out the menu bar: File, Edit, Selection, View, Go, Run, Terminal, Help) and a round close pill at the right (no minimise / maximise buttons; double-click the title bar to maximise), maximize on launch, compositor blur behind the chrome.

## Shortcuts
| keys | action |
|---|---|
| Ctrl+, | settings |
| Ctrl+O / Ctrl+Shift+O | open file / open folder |
| Ctrl+N, Ctrl+S, Ctrl+Shift+S, Ctrl+W, Ctrl+Q | new, save, save as, close tab, quit |
| Ctrl+B / Ctrl+Shift+E | toggle sidebar / focus explorer (then arrows, Enter, F2, Del, Ctrl+C/X/V, F5) |
| Ctrl+Shift+F / Ctrl+Shift+H | project search / replace |
| Ctrl+Shift+G | Git view; Ctrl+Shift+M Problems; Ctrl+Shift+P projects |
| Ctrl+` | integrated terminal |
| Ctrl+F9 / Shift+F10 / Shift+F9 | build / build and run / build and debug |
| F5, Shift+F5, Ctrl+Shift+F5, F6 | debug: continue/start, stop, restart, pause |
| F9, F10, F11, Shift+F11, Ctrl+F10, Ctrl+Shift+F10 | breakpoint, step over, into, out, run to cursor, set next statement |
| Ctrl+F2, Ctrl+Shift+R | stop run, reload CMake |
| Ctrl+Z/Y, Ctrl+/, Ctrl+D/K (Shift), Alt+Up/Down, Ctrl+]/[ | undo/redo, comment, duplicate/delete line, move line, indent |
| F12, Alt+Left, Alt+O, Ctrl+Space | definition, back, header/source, completion |
| Ctrl+Tab, Ctrl+PgUp/PgDn | switch tabs |

**Settings**: `libs/services/settings.*` (the table of settings, validation, `settings.json`), `app/impl/settings_view.cpp` (dialog and applying changes to the running IDE). To add a setting: one entry in `defs()`, then read it with `cfgB/cfgI/cfgS` where the feature lives; the dialog and the file format pick it up by themselves.
