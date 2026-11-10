# Agent guide (read this first)

Daemon is a Linux-only IDE: a native **Wayland** client with a **Vulkan 1.3** renderer (no UI toolkit), written in
**C++23 with C++20 modules** (`import std;`). Start here, then read `HANDOFF.md` (state + next steps) and `docs/`.

| Document | Read it for |
|---|---|
| `HANDOFF.md` | current state, what was verified, what was not, suggested next steps |
| `docs/DEVELOPMENT.md` | build, tests, screenshot/scripting hooks, recipes, compiler quirks |
| `docs/ARCHITECTURE.md` | libraries, modules, `App` structure, event loop, rendering, async patterns |
| `docs/SUBSYSTEMS.md` | clangd/CMake, git, debugger, search, terminal, file watching, projects |
| `docs/USER_GUIDE.md` | detailed user-facing reference (keys, git, debugger, search, terminal, modules) |
| `docs/FEATURES.md` | everything the IDE does, with shortcuts and where each feature lives |
| `docs/DESIGN.md` | visual language, theme values, what the owner accepted or rejected |
| `docs/KNOWN_ISSUES.md` | limitations, untested paths, roadmap |

## Ground rules
1. **Build and test after every change**: `cmake --build build && ./build/tests/daemon_tests` (must print `all tests passed`).
2. **Look at the result.** UI work is judged visually; use `--screenshot` (see `docs/DEVELOPMENT.md`) and read the PNG.
3. **Commit each finished change** with a clear message. Keep commits focused; do not rewrite history.
4. **Never kill processes by name or pattern** (only by PID you started). Keep temp files out of the repo.
5. **Do not reintroduce rejected designs** (list in `docs/DESIGN.md`). The owner iterates on looks and will say "revert it".
6. **Report honestly**: say what you tested and what you could not (mouse interaction cannot be scripted).
7. Match the existing code style; prefer small functions; put new member functions in the matching `app/impl/*.cpp`.
8. Where to put things: UI-framework code goes to `libs/ui` (no IDE knowledge), editor model to `libs/text`, tools/protocol
   clients to `libs/services`, anything that touches `App` state to `app/`.
