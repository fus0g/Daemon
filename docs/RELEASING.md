# Versioning and releases

Daemon follows [Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`.

- While `MAJOR` is 0 (now) the project is in initial development: any release may change behaviour. Bump **MINOR** for new
  features or notable changes, **PATCH** for bug fixes only. `1.0.0` means the core feature set is stable and the owner is happy to
  support it (daily-driver quality, no known data-loss bugs, Setup/debugger/search hand-tested).
- The version number lives in **one place**: `project(daemon VERSION x.y.z ...)` in the top-level `CMakeLists.txt`. The build turns it
  into `ide::version()` (welcome screen) and `ide::versionFull()` (`daemon --version`). Builds from commits that are not tagged
  show `x.y.z+<git-hash>` (and `.dirty` with uncommitted changes).

## Cutting a release
1. Make sure `cmake --build build && ./build/tests/daemon_tests` is green and the working tree is clean.
2. Move the `[Unreleased]` notes in `CHANGELOG.md` under a new `## [x.y.z] - YYYY-MM-DD` heading.
3. Change the version in `CMakeLists.txt`.
4. `git commit -am "Release x.y.z"` then `git tag -a vx.y.z -m "Daemon x.y.z"` (tag name = `v` + version).
5. Re-run CMake (`cmake -S . -B build`) and check `./build/app/daemon --version` prints `Daemon x.y.z` without a `+hash`.
6. Push the commit and the tag (`git push && git push --tags`) once a remote exists.

## Packages (.deb, .rpm, .AppImage)
```
tools/package.sh [deb|rpm|appimage|all]        # or: cmake --build build --target package-deb|package-rpm|package-appimage|packages
```
Output goes to `dist/` (git-ignored): `daemon-ide_<ver>-1_amd64.deb`, `daemon-ide-<ver>-1.<arch>.rpm`, `Daemon-<ver>-<arch>.AppImage`.
Untagged commits get a pre-release suffix (`0.1.0~git<hash>`, rpm release `0.1.git<hash>`), tagged ones are clean.

How it works: a Release build in `build-release/`, installed into `build-release/stage/usr`, stripped (about 6.5 MB), plus the GCC
runtime and Wayland/xkbcommon libraries copied to `usr/lib/daemon` (the executable's RUNPATH is `$ORIGIN/../lib/daemon`). `compat.c`
redirects `sqrtf`/`acosf` to the double versions so the binary only needs **glibc >= 2.38** (Ubuntu 24.04+, Debian 13+, Fedora 39+,
Arch, openSUSE Tumbleweed). The .deb is assembled with `ar`/`tar` (no dpkg needed); the .rpm with `rpmbuild`; the AppImage with
`appimagetool`, downloaded once to `~/.cache/daemon-packaging` and verified against the SHA-256 GitHub publishes (needs FUSE 2 on the
user's machine to run, or `--appimage-extract-and-run`).
Installed packages provide `/usr/share/applications/daemon.desktop`, so the first-start self-registration (`installDesktopEntry`) is skipped;
inside an AppImage it points the launcher at the `.AppImage` file (`$APPIMAGE`).
Vulkan itself and a GPU driver always come from the system (`libvulkan1` / `vulkan-loader` dependency).
