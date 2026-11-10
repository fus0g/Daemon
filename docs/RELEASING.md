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
