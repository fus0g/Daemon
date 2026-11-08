# Known issues, untested paths, roadmap

## Verified by the previous agent
Unit tests (all pass) and screenshots with scripted keys: editor, explorer, search/replace on a scratch folder, terminal (`ls`, `echo`,
Ctrl+L), welcome/new-project dialogs, debugger sessions on a small CMake project (breakpoints, conditional + logpoint, watch, stepping,
frames, variable expansion, console evaluation, clean exit), file watching (external create/modify), restructured build.

## Not verified with real input (scripts cannot move a mouse)
Explorer drag-and-drop moves · terminal mouse selection/paste/vim/htop · rename/delete via the sidebar card · zenity folder flow ·
debugger: gutter click, right-click editor menu, hover action buttons (edit value, add watch, data breakpoint, remove), threads,
C++ throw breakpoint, set next statement · search result hover buttons (replace/dismiss) and Replace All confirmation ·
the welcome screen "disappears and comes back" report (fixed by keeping it visible while the sidebar prompt is open; unconfirmed).

## Known limitations
- Breakpoints keep their original line numbers when the file is edited above them.
- Logpoints stop briefly (the gdb console shows "Breakpoint N, ..." lines for them).
- No disassembly/registers/memory views (use the Debug console: `disassemble`, `info registers`, `x/16x ptr`).
- Replace-in-files writes to disk and skips files with unsaved edits; undo history of an open file is reset when it reloads.
- Project search does not search inside symlinked directories differently from `git ls-files`; very large result sets are capped at 20 000 matches.
- `App` is a single large class (implementation split by feature). Further decomposition (debugger/search/terminal as separate
  classes with narrow interfaces) is possible but is a design change, not a file move.
- Folder chooser needs `zenity`; otherwise the built-in browser is used.
- clangd must be installed for intelligence; gdb for debugging; git for the Git features.

## Compiler constraints
See "Compiler quirks" in `docs/DEVELOPMENT.md` (GCC 16 module bugs; clean the build dir if module files look corrupt).

## Roadmap ideas (none started)
Clone-from-URL on the welcome screen · terminal tabs/splits · in-editor find overlay instead of the bottom prompt bar ·
breakpoint line tracking on edits · disassembly/registers panels · test explorer · formatting (clang-format) · rename symbol / code
actions through LSP · settings UI and keymap customisation · Windows/macOS are out of scope (Wayland + Vulkan only).
