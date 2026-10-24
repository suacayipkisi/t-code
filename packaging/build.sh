#!/usr/bin/env bash
# Builds the t-code packages into packaging/packages/<distro>/:
#   fedora    .rpm (+ .src.rpm), with rpmbuild on this machine when it runs Fedora, else in a container
#   arch      .pkg.tar.zst, in an Arch Linux container
#   debian    .deb, in a Debian 13 (trixie) container
#   void, void-musl  .xbps, in Void Linux containers (glibc and musl)
#   appimage  .AppImage, in an Ubuntu 24.04 container
# Usage: packaging/build.sh [fedora|arch|debian|void|void-musl|appimage|all]...   (default: all)
# packaging/packages/release/ then holds just the packages to publish (no debug or source
# packages) with their SHA256SUMS. packaging/test.sh installs them in fresh containers and checks
# that they work.
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

build_fedora() {
  local out="$OUT/fedora"
  rm -rf "$out" && mkdir -p "$out"
  if [[ "$(. /etc/os-release && echo "$ID")" == fedora ]] && command -v rpmbuild >/dev/null; then
    local top
    top="$(mktemp -d)"
    trap 'rm -rf "$top"' RETURN
    make_tarball "$top/SOURCES"
    info "Building the RPM with this machine's rpmbuild"
    rpmbuild -ba --define "_topdir $top" "$PACKAGING/fedora/t-code.spec" ||
      die "rpmbuild failed (missing build dependencies? sudo dnf builddep packaging/fedora/t-code.spec)"
    cp "$top"/RPMS/*/*.rpm "$top"/SRPMS/*.rpm "$out/"
  else
    make_tarball "$out"
    info "Building the RPM in ${IMAGE[fedora]}"
    run_in_container fedora "$out" bash /src/packaging/fedora/build-in-container.sh
    rm -f "$out/t-code-$VERSION.tar.gz"
  fi
}

build_arch() {
  local out="$OUT/arch"
  rm -rf "$out" && make_tarball "$out"
  info "Building the Arch package in ${IMAGE[arch]}"
  run_in_container arch "$out" bash /src/packaging/arch/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

build_debian() {
  local out="$OUT/debian"
  rm -rf "$out" && make_tarball "$out"
  info "Building the Debian package in ${IMAGE[debian]}"
  run_in_container debian "$out" bash /src/packaging/debian/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

build_appimage() {
  local out="$OUT/appimage"
  rm -rf "$out" && make_tarball "$out"
  info "Building the AppImage in ${IMAGE[appimage]}"
  run_in_container appimage "$out" bash /src/packaging/appimage/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

# The installable package of each distribution built for this version, side by side for a release.
collect_release() {
  local release="$OUT/release" files=()
  rm -rf "$release" && mkdir -p "$release"
  for f in "$OUT/fedora/t-code-$VERSION-"[0-9]*.x86_64.rpm \
           "$OUT/arch/t-code-$VERSION-"[0-9]*-x86_64.pkg.tar.zst \
           "$OUT/debian/t-code_$VERSION-"[0-9]*_amd64.deb \
           "$OUT/void/t-code-${VERSION}_"[0-9]*.x86_64.xbps \
           "$OUT/void-musl/t-code-${VERSION}_"[0-9]*.x86_64-musl.xbps \
           "$OUT/appimage/t-code-$VERSION-x86_64.AppImage"; do
    [[ -f "$f" ]] && files+=("$f")
  done
  ((${#files[@]})) || return 0
  cp "${files[@]}" "$release/"
  (cd "$release" && sha256sum -- * > SHA256SUMS)
}

build_void() { # <void or void-musl>
  local out="$OUT/$1"
  rm -rf "$out" && make_tarball "$out"
  info "Building the Void Linux package in ${IMAGE[$1]}"
  run_in_container "$1" "$out" bash /src/packaging/void/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

targets=("$@")
((${#targets[@]})) || targets=(all)
[[ " ${targets[*]} " == *" all "* ]] && targets=("${DISTROS[@]}")
check_versions
for distro in "${targets[@]}"; do
  case "$distro" in
    fedora | arch | debian) "build_$distro" ;;
    appimage) build_appimage ;;
    void | void-musl) build_void "$distro" ;;
    *) die "Unknown target '$distro' (fedora, arch, debian, void, void-musl, appimage or all)" ;;
  esac
done
collect_release
info "Packages (t-code $VERSION):"
find "$OUT" -path "$OUT/release" -prune -o -type f \
  \( -name '*.rpm' -o -name '*.pkg.tar.*' -o -name '*.deb' -o -name '*.xbps' -o -name '*.AppImage' \) \
  -print | sort | sed "s|^$ROOT/|  |"
info "To publish (packaging/packages/release/):"
ls "$OUT/release" | sed 's|^|  |'
