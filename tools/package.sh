#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds distributable packages of Daemon: .deb and .rpm  ->  dist/
#
#   tools/package.sh [deb|rpm|all]      (default: all)
#
# One Release build is made in build-release/, installed into a staging tree together with the GCC runtime it was built with
# (the executable finds it through its RUNPATH), and then wrapped by each packager. Needs: cmake, ninja, strip, tar, xz, ar.
# rpm needs `rpmbuild`.
# The result needs glibc >= 2.38 (Ubuntu 24.04+, Debian 13+, Fedora 39+, Arch, openSUSE Tumbleweed).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TARGETS=("${@:-all}")
[[ "${TARGETS[0]}" == all ]] && TARGETS=(deb rpm)

VERSION="$(sed -n 's/^project(daemon VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[[ -n "$VERSION" ]] || { echo "cannot read the version from CMakeLists.txt" >&2; exit 1; }
HASH="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
if git describe --tags --exact-match --match "v$VERSION" >/dev/null 2>&1; then
  DEB_VERSION="$VERSION-1"; RPM_RELEASE="1"
else   # untagged commit: sorts before the real release
  DEB_VERSION="$VERSION~git$HASH-1"; RPM_RELEASE="0.1.git$HASH"
fi
case "$(uname -m)" in
  x86_64)  DEB_ARCH=amd64; RPM_ARCH=x86_64 ;;
  aarch64) DEB_ARCH=arm64; RPM_ARCH=aarch64 ;;
  *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
MAINTAINER="$(git config user.name 2>/dev/null || echo Daemon) <$(git config user.email 2>/dev/null || echo noreply@example.invalid)>"
DESCRIPTION="Native Wayland + Vulkan C++ IDE"
LONGDESC="Daemon is a code editor and IDE for Linux with clangd intelligence (C++20 modules aware), CMake projects, a gdb debugger, git, project search and an integrated terminal."
DIST="$ROOT/dist"; BUILD="$ROOT/build-release"; STAGE="$ROOT/build-release/stage"
mkdir -p "$DIST"

echo "== Building Daemon $VERSION ($HASH) Release"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --target daemon

echo "== Staging"
rm -rf "$STAGE"
DESTDIR="$STAGE" cmake --install "$BUILD" --prefix /usr >/dev/null
strip --strip-unneeded "$STAGE/usr/bin/daemon"
# The GCC runtime and the small Wayland libraries travel with the program (RUNPATH $ORIGIN/../lib/daemon).
mkdir -p "$STAGE/usr/lib/daemon"
for lib in libstdc++.so.6 libgcc_s.so.1 libwayland-client.so.0 libwayland-cursor.so.0 libxkbcommon.so.0; do
  path="$(ldd "$STAGE/usr/bin/daemon" | awk -v l="$lib" '$1==l {print $3}')"
  [[ -n "$path" ]] && cp -L "$path" "$STAGE/usr/lib/daemon/$lib"
done
install -Dm644 LICENSE "$STAGE/usr/share/doc/daemon/copyright"
install -Dm644 THIRD_PARTY_NOTICES.md "$STAGE/usr/share/doc/daemon/THIRD_PARTY_NOTICES.md"
install -Dm644 CHANGELOG.md "$STAGE/usr/share/doc/daemon/CHANGELOG.md"
sed -i 's|^Exec=daemon %F|Exec=/usr/bin/daemon %F|' "$STAGE/usr/share/applications/daemon.desktop"
echo "   $(du -sh "$STAGE" | cut -f1) staged"

# ------------------------------------------------------------------ .deb
make_deb() {
  local out="$DIST/daemon-ide_${DEB_VERSION}_${DEB_ARCH}.deb" w="$BUILD/deb"
  rm -rf "$w"; mkdir -p "$w/control"
  local size; size=$(du -sk "$STAGE" | cut -f1)
  cat > "$w/control/control" <<EOF
Package: daemon-ide
Version: $DEB_VERSION
Architecture: $DEB_ARCH
Maintainer: $MAINTAINER
Installed-Size: $size
Depends: libc6 (>= 2.38), libvulkan1, libwayland-client0, libwayland-cursor0, libxkbcommon0
Recommends: clangd, cmake, ninja-build, gdb, git, mesa-vulkan-drivers
Section: devel
Priority: optional
Homepage: https://example.invalid/daemon
Description: $DESCRIPTION
 $LONGDESC
EOF
  ( cd "$STAGE" && find usr -type f -print0 | sort -z | xargs -0 md5sum ) > "$w/control/md5sums"
  tar --owner=0 --group=0 --numeric-owner -C "$w/control" -cJf "$w/control.tar.xz" .
  tar --owner=0 --group=0 --numeric-owner -C "$STAGE" -cJf "$w/data.tar.xz" usr
  echo "2.0" > "$w/debian-binary"
  rm -f "$out"
  ( cd "$w" && ar rc "$out" debian-binary control.tar.xz data.tar.xz )
  echo "   $out"
}

# ------------------------------------------------------------------ .rpm
make_rpm() {
  command -v rpmbuild >/dev/null || { echo "rpmbuild not found (install rpm-build) - skipping rpm" >&2; return 0; }
  local w="$BUILD/rpm"; rm -rf "$w"; mkdir -p "$w"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
  cat > "$w/SPECS/daemon-ide.spec" <<EOF
%global debug_package %{nil}
%global __strip /bin/true
%global __requires_exclude ^(libstdc\+\+|libgcc_s).*
%global __provides_exclude_from ^%{_prefix}/lib/daemon/.*
Name:           daemon-ide
Version:        $VERSION
Release:        $RPM_RELEASE
Summary:        $DESCRIPTION
License:        GPL-3.0-or-later
URL:            https://example.invalid/daemon
Requires:       vulkan-loader
Recommends:     clang-tools-extra cmake ninja-build gdb git mesa-vulkan-drivers
%description
$LONGDESC

%install
mkdir -p %{buildroot}
cp -a $STAGE/usr %{buildroot}/usr

%files
%license %{_docdir}/daemon/copyright
%doc %{_docdir}/daemon/THIRD_PARTY_NOTICES.md
%doc %{_docdir}/daemon/CHANGELOG.md
%{_bindir}/daemon
%{_prefix}/lib/daemon
%{_datadir}/applications/daemon.desktop
%{_datadir}/icons/hicolor/*/apps/daemon.*
EOF
  rpmbuild -bb --define "_topdir $w" --target "$RPM_ARCH" "$w/SPECS/daemon-ide.spec" >"$w/rpmbuild.log" 2>&1 || { tail -20 "$w/rpmbuild.log" >&2; return 1; }
  cp "$w"/RPMS/*/*.rpm "$DIST/"
  echo "   $(ls "$DIST"/daemon-ide-"$VERSION"-"$RPM_RELEASE"*.rpm)"
}

for t in "${TARGETS[@]}"; do
  echo "== Packaging $t"
  case "$t" in
    deb) make_deb ;; rpm) make_rpm ;;
    *) echo "unknown target '$t' (deb|rpm|all)" >&2; exit 2 ;;
  esac
done
echo "== Done: $(ls -1 "$DIST" | tr '\n' ' ')"
