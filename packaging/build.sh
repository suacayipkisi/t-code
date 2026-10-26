#!/usr/bin/env bash
# Builds packages under packaging/packages/<distro>/ and collects release artifacts.
# Usage: packaging/build.sh [fedora|arch|aur|debian|void|void-musl|flatpak|snap|all]...
# (default: all). packaging/test.sh installs native packages in fresh containers and smoke-tests
# sandboxed formats through their format-specific launchers.
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

build_aur() {
  local out="$OUT/aur"
  rm -rf "$out" && make_tarball "$out"
  info "Building the AUR source package in ${IMAGE[aur]}"
  run_in_container aur "$out" bash /src/packaging/aur/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

build_flatpak() {
  local out="$OUT/flatpak"
  rm -rf "$out" && mkdir -p "$out"
  info "Building the Flatpak bundle from this working tree"
  bash "$PACKAGING/flatpak/build.sh" --local-source "$ROOT" "$out"
}

build_snap() {
  local out="$OUT/snap"
  rm -rf "$out" && mkdir -p "$out"
  info "Building the strict Snap in LXD"
  bash "$PACKAGING/snap/build.sh" "$ROOT" "$out"
}

build_debian() {
  local out="$OUT/debian"
  rm -rf "$out" && make_tarball "$out"
  info "Building the Debian package in ${IMAGE[debian]}"
  run_in_container debian "$out" bash /src/packaging/debian/build-in-container.sh
  rm -f "$out/t-code-$VERSION.tar.gz"
}

# Installable release packages, plus Snapcraft provenance; source/debug packages stay out.
collect_release() {
  local release="$OUT/release" files=()
  rm -rf "$release" && mkdir -p "$release"
  for f in "$OUT/fedora/t-code-$VERSION-"[0-9]*.x86_64.rpm \
           "$OUT/arch/t-code-$VERSION-"[0-9]*-x86_64.pkg.tar.zst \
           "$OUT/debian/t-code_$VERSION-"[0-9]*_amd64.deb \
           "$OUT/void/t-code-${VERSION}_"[0-9]*.x86_64.xbps \
           "$OUT/void-musl/t-code-${VERSION}_"[0-9]*.x86_64-musl.xbps \
           "$OUT/flatpak/t-code-$VERSION-"*.flatpak \
           "$OUT/snap/t-code_${VERSION}_amd64.snap"; do
    [[ -f "$f" ]] && files+=("$f")
  done
  if [[ -f "$OUT/snap/t-code_${VERSION}_amd64.snap" ]]; then
    for f in "$OUT/snap/build-environment.txt" "$OUT/snap/build-manifest.yaml"; do
      [[ -f "$f" ]] || die "Missing Snap build provenance: $f"
      files+=("$f")
    done
  fi
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
    fedora | arch | aur | debian) "build_$distro" ;;
    void | void-musl) build_void "$distro" ;;
    flatpak) build_flatpak ;;
    snap) build_snap ;;
    *) die "Unknown target '$distro' (fedora, arch, aur, debian, void, void-musl, flatpak, snap or all)" ;;
  esac
done
collect_release
info "Packages (t-code $VERSION):"
find "$OUT" -path "$OUT/release" -prune -o -type f \
  \( -name '*.rpm' -o -name '*.pkg.tar.*' -o -name '*.deb' -o -name '*.xbps' \
     -o -name '*.flatpak' -o -name '*.snap' \) \
  -print | sort | sed "s|^$ROOT/|  |"
info "To publish (packaging/packages/release/):"
ls "$OUT/release" | sed 's|^|  |'
