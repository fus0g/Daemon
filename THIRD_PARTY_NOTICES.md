# Third-party notices

Daemon itself is GPL-3.0-or-later (`LICENSE`). It bundles the following third-party files, all of which are
compatible with GPLv3 and keep their own licences:

| Path | What | Licence |
|---|---|---|
| `third_party/stb/stb_truetype.h` | stb_truetype v1.26 (Sean Barrett) | Public domain / MIT (dual, text at the end of the file) |
| `third_party/fonts/materialdesignicons.ttf` | Material Design Icons font (Pictogrammers) | Apache-2.0 for the font/icons (MIT for non-font files); full terms in `third_party/fonts/LICENSE-materialdesignicons.txt` |
| `third_party/wayland/xdg-shell.xml` | xdg-shell protocol | MIT-style (text in the file header) |
| `third_party/wayland/xdg-decoration-unstable-v1.xml` | xdg-decoration protocol | MIT-style (text in the file header) |
| `third_party/wayland/ext-background-effect-v1.xml` | ext-background-effect protocol | MIT-style (text in the file header) |
| `third_party/wayland/xdg-toplevel-icon-v1.xml` | xdg-toplevel-icon protocol | MIT-style (text in the file header) |

Apache-2.0 material is compatible with GPLv3 (it is not compatible with GPLv2-only). Keep the licence texts and
copyright notices above whenever you redistribute these files.

Not bundled, used at build/run time as system components: Wayland, xkbcommon, Vulkan loader/headers, the system
monospace font (e.g. Source Code Pro, OFL), clangd, gdb, git and CMake. They are not redistributed with this repository.
Generated protocol code (from the XML above) is produced at build time.
