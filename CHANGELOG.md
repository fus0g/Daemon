# Changelog

All notable changes to Daemon are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
project uses [Semantic Versioning](https://semver.org/) (see `docs/RELEASING.md`).

## [Unreleased]

## [0.8.0] - 2026-11-16

### Added
- GLSL inside C++ raw strings: `R"(...)"` / `R"GLSL(...)GLSL"` literals that hold a shader (recognised by `#version`, `gl_` variables, `void main` with uniforms / layouts, or a name such as `fragSrc` / `glsl` / `shader` in front) are highlighted with the GLSL rules, and get the same IntelliSense as a shader file: completion (struct members, swizzles, built-ins of the stage guessed from the name or contents), parameter hints, hover, F12 and error checking with glslc, with the messages on the right lines of the C++ file. Raw strings of any other kind are now highlighted as one multi-line string.
- Typing `}` on a line that only holds indentation moves it back one level (C, C++, GLSL, Java, JavaScript, TypeScript, Go, Rust).

### Fixed
- Shader error checking for OpenGL-style shaders no longer reports plain `uniform mat4 m;` as an error.

## [0.7.0] - 2026-11-16

### Added
- GLSL shaders: syntax highlighting for every shader extension (`.glsl .vert .frag .geom .tesc .tese .comp .mesh .task .rgen .rint .rahit .rchit .rmiss .rcall .vs .fs .tcs .tes .vsh .fsh .glslv .glslf ...`) and IntelliSense that needs no language server: completion of keywords, types, built-in functions and the built-in variables of the shader's stage (`gl_FragCoord` only in fragment shaders), the functions, structs, uniforms, blocks, macros, parameters and locals of the file, members after `.` (struct fields, block members, swizzles `xyz` / `rgb` / `stp`), `#` directives and `layout(...)` qualifiers; parameter hints for built-ins, your functions, struct and vector constructors; hover text; go to definition (F12); the structure view; and error checking with glslc or glslangValidator while you type (squiggles, Problems tab; Settings > Shaders chooses the target API). glslc is listed in Settings > Tools.

## [0.6.1] - 2026-11-16

### Changed
- Structure view icons are plainer: no coloured tiles, softer colours (one hue each for types, functions and data, grey for the rest).

## [0.6.0] - 2026-11-16

### Added
- Structure view: under the file explorer, the symbols of the current file as a tree - namespaces, classes, structs, enums, functions, methods, fields, constants (from clangd) or the headings of a Markdown file - each with a coloured icon, its type or signature in grey, and the symbol the cursor is in marked. Click to jump there, the arrow folds a branch, the header collapses the whole view and the divider above it can be dragged. Settings: Appearance > Structure view / Structure view height.

## [0.5.0] - 2026-11-16

### Added
- Parameter hints: while the cursor is inside a call, a small panel above it shows the signature with the parameter you are typing underlined, its type and name, the return type, and the parameter's documentation when there is some (more than one overload: "1/3"). It appears when you accept a function from the completion list, when you type `(` or `,`, and follows the cursor until you leave the call (Esc closes it). Works in notebook cells too.
- Completion list: a footer under the list shows the selected entry in full - return type, name and every parameter with its type - for functions, and for anything the list had to cut short.

## [0.4.2] - 2026-11-14

### Changed
- Completion order: types (classes, structs, enums, typedefs) come first, then namespaces and keywords, functions, variables and fields, and macros last; within that a same-case prefix beats a case-insensitive one, then the capitals of the name (`gw` finds `GLFWwindow`), then the server's own ranking. Things you accepted before come first. After `.` and `->` the server's order is kept. A name that is both a typedef and a struct is listed once.

### Fixed
- Autocomplete showed the wrong things when the list was long: clangd cut it at 100 entries and Daemon only filtered that old list while you typed on, so `GLFWwi` showed one unrelated macro and never `GLFWwindow`. Cut-off lists are now requested again for the longer prefix, and the limit is 1000.

## [0.4.1] - 2026-11-14

### Changed
- The toolchain check moved from its own dialog into Settings > Tools (Help > Check Toolchain... and the start-up reminder open that page; the welcome screen's "Check Tools" became "Settings"). "Don't remind me" is now the setting Tools > Remind me about missing tools (an earlier choice is carried over).

## [0.4.0] - 2026-11-14

### Added
- Settings (Ctrl+, or File > Settings...): a dialog with search and sections for everything Daemon can be configured for - editor (font size, tab width, spaces/tabs, indentation detection, closing brackets, minimap, breadcrumbs, cursor blink, reload on change), appearance (interface font size, animations, start maximized), general (reopen last project, restore tabs, tool reminders), git (change markers, blame), clangd (on/off, program, background index), terminal shell, notebook compiler and flags, Markdown web images. Changes apply at once and are saved to `~/.config/daemon/settings.json`, which can also be edited by hand and is reloaded when it changes.

### Changed
- Zooming the editor font (Ctrl+= / Ctrl+-) is saved as the font size setting.

## [0.3.1] - 2026-11-14

### Fixed
- Many open tabs no longer run out of the tab strip over the sidebar: tabs shrink to a minimum width, then the strip shows a window of tabs that follows the current one (mouse wheel over the strip scrolls it).
- Sessions only remember and restore files inside their own project. Switching projects no longer carries the previous project's tabs over (unsaved tabs are kept), and old sessions that contain other projects' files are cleaned up when restored.

## [0.2.0] - 2026-11-14

### Fixed
- Closing the window no longer waits for background jobs (a slow `git fetch`, a picture download) or a running notebook compile.
- Notebook completion could stay silent when you typed right after opening: clangd answered an empty list while it was still parsing
  and nothing asked again. It now starts when the notebook is shown, retries an empty answer while it warms up, and the toolbar says
  "IntelliSense: warming up" (or that it is off when clangd is missing). The toolbar also no longer overlaps its buttons in narrow panes.
- Notebooks: after clicking in a cell the caret kept following the mouse pointer (the end of the drag was never noticed), so the arrow
  keys seemed dead. The drag now ends when the button is released or a key is pressed.
- Notebooks: a failed cell no longer prints hundreds of lines of GCC output (every overload candidate and header in the include
  chain); only the errors, warnings and the first notes remain, and notebooks saved with the long form are shortened when opened.
- Stale clangd errors (`'xyz.h' file not found` for headers generated by the build, `module 'std' not found` while clangd was still
  building its module cache) stayed in the editor and the Problems tab although the project built fine. Files with such errors
  are now re-checked after every build and a few times shortly after clangd starts.

### Changed
- Title bar: the minimise and maximise buttons are gone; the logo (left, shows the menu bar) and the close button (right) are round pills like the activity bar.

### Added
- Notebook IntelliSense: completion, error squiggles and hover inside cells (clangd over a virtual file made of all code cells);
  the toolbar shows whether the notebook is in edit or command mode.
- C++ notebooks: `.dcxxnb` (Daemon's own name for the same nbformat-4 JSON) and `.ipynb` files open as notebooks with code and markdown cells, run on a built-in kernel (no Jupyter): every cell is
  compiled with GCC as a C++20 module and loaded into a persistent runner, so state carries over, `import std;` and user modules
  work. Toolbar and Jupyter-style command/edit modes, outputs and values under the cells, compile errors with cell positions, stop /
  restart, `%flags` / `%link` magics, `daemon --nb-run`. Needs GCC 15+.
- Problems tab: **Copy all** (and **Copy errors** when warnings are mixed in) puts every problem on the clipboard as `file:line:col: error: message` lines.
- Markdown preview: `Ctrl+Shift+V` (or the editor context menu) opens a live, read-only rendered view of a `.md` file beside it - headings, emphasis,
  code with syntax colours, tables, nested and task lists, quotes, links (web, anchors, relative files) and real images
  (PNG, JPEG, GIF, BMP, SVG incl. shields.io badges; local files and https URLs fetched with curl into `~/.cache/daemon/images`;
  `DAEMON_NO_REMOTE_IMAGES=1` disables downloads). Needs a new colour picture atlas in the Vulkan renderer (allocated only while
  a preview shows pictures) and bundles stb_image and NanoSVG (see `THIRD_PARTY_NOTICES.md`).
- Read-only buffers (`Buffer::readOnly`), a hand cursor over links.
- Sessions: the open files, split layout, active tab, cursor and scroll positions of each project are saved
  (`~/.local/state/daemon/sessions/`) and restored when the project is opened. A plain launch reopens the last project.
  Untitled buffers are not restored; unsaved edits are never replaced.

## [0.1.1] - 2026-11-12

### Fixed
- Window stayed frozen or ignored the mouse and keyboard after suspend/resume: the pointer and keyboard are now requested
  again when the compositor withdraws and re-announces them.
- Renderer recovers from a lost Vulkan device or surface by rebuilding itself, restarts after a resume, and no longer exits
  when frames fail for a while; a stall watchdog rebuilds it if frames stay stuck.
- Bounded fence and acquire waits so a stalled GPU cannot freeze the UI thread.

### Added
- Log file at `~/.local/state/daemon/daemon.log` (start-up, suspend/resume, renderer rebuilds, seat changes, stalls).

## [0.1.0] - 2026-11-11

First tagged release.

### Added
- Native Wayland + Vulkan 1.3 window with translucent, blurred chrome; layered C++23 module code base (`libs/` + `app/`).
- Editor: tabs, split panes, drag and drop, minimap, breadcrumbs, syntax highlighting, find/replace, animated caret.
- C/C++ intelligence through clangd, including C++20 modules (`import std;`), project-wide problems.
- CMake projects: configure, build, run, profiles, targets, clickable compiler errors.
- gdb debugger: breakpoints (conditional, logpoints, data, function, C++ throw), stepping, variables, watches, call stack, console.
- Git integration, project-wide search and replace, integrated terminal, file explorer with drag-to-move and live updates.
- Welcome screen, new-project wizard, system folder chooser.
- Toolchain check at start-up with distro-specific install commands and verified one-click downloads of clangd, CMake and Ninja.
- Dragon-winged helm logo, launcher entry and icons registered automatically.
- GPL-3.0-or-later licence, third-party notices, handoff and architecture documentation.

### Performance
- Prefers the integrated GPU, frame-paced presentation, bounded clangd resource use, idle editor does not wake up.

[Unreleased]: https://example.invalid/compare/v0.8.0...HEAD
[0.8.0]: https://example.invalid/compare/v0.7.0...v0.8.0
[0.7.0]: https://example.invalid/compare/v0.6.1...v0.7.0
[0.6.1]: https://example.invalid/compare/v0.6.0...v0.6.1
[0.6.0]: https://example.invalid/compare/v0.5.0...v0.6.0
[0.5.0]: https://example.invalid/compare/v0.4.2...v0.5.0
[0.4.2]: https://example.invalid/compare/v0.4.1...v0.4.2
[0.4.1]: https://example.invalid/compare/v0.4.0...v0.4.1
[0.4.0]: https://example.invalid/compare/v0.3.1...v0.4.0
[0.3.1]: https://example.invalid/compare/v0.3.0...v0.3.1
[0.3.0]: https://example.invalid/compare/v0.2.0...v0.3.0
[0.2.0]: https://example.invalid/compare/v0.1.1...v0.2.0
[0.1.1]: https://example.invalid/compare/v0.1.0...v0.1.1
[0.1.0]: https://example.invalid/releases/tag/v0.1.0
