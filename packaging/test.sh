#!/usr/bin/env bash
# Installs native packages in fresh distro containers and smoke-tests sandboxed formats through
# their format-specific launchers.
# Usage: packaging/test.sh [fedora|arch|aur|debian|void|void-musl|flatpak|snap|all]...
# (default: all)
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# Installs the package, then what smoke-test.sh needs: a headless X server, a session bus,
# python3 (to talk to the socket) and pgrep. Only this version's package ($VERSION, set in the
# container), when older ones are still in packaging/packages.
declare -A INSTALL=(
  ["fedora"]='dnf -y install $(ls /out/t-code-ide-$VERSION-[0-9]*.x86_64.rpm) xorg-x11-server-Xvfb dbus-daemon
            python3 procps-ng desktop-file-utils fontconfig util-linux'
  ["arch"]='pacman -Syu --noconfirm && pacman -U --noconfirm $(ls /out/t-code-ide-$VERSION-[0-9]*-x86_64.pkg.tar.*) &&
          pacman -S --noconfirm --needed xorg-server-xvfb dbus python procps-ng desktop-file-utils'
  ["aur"]='pacman -Syu --noconfirm && pacman -U --noconfirm $(ls /out/t-code-ide-$VERSION-[0-9]*-x86_64.pkg.tar.*) &&
          pacman -S --noconfirm --needed xorg-server-xvfb dbus python procps-ng desktop-file-utils'
  ["debian"]='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y
            $(ls /out/t-code-ide_$VERSION-[0-9]*_amd64.deb) xvfb dbus python3
            procps desktop-file-utils'
  # A local repository: xbps installs from repositories only, not from a package file.
  ["void"]='xbps-install -Syu xbps && xbps-install -Syu && mkdir -p /tmp/repo &&
          cp /out/t-code-ide-${VERSION}_[0-9]*.xbps /tmp/repo/ && xbps-rindex -a /tmp/repo/*.xbps &&
          xbps-install -y --repository=/tmp/repo t-code-ide xorg-server-xvfb dbus python3 procps-ng
          desktop-file-utils fontconfig util-linux dejavu-fonts-ttf'
  ["void-musl"]='xbps-install -Syu xbps && xbps-install -Syu && mkdir -p /tmp/repo &&
          cp /out/t-code-ide-${VERSION}_[0-9]*.xbps /tmp/repo/ && xbps-rindex -a /tmp/repo/*.xbps &&
          xbps-install -y --repository=/tmp/repo t-code-ide xorg-server-xvfb dbus python3 procps-ng
          desktop-file-utils fontconfig util-linux dejavu-fonts-ttf'
)
# Void has no package for the Cascadia Code font: t-code falls back to the default monospace one
# (a bare container has no font at all, hence DejaVu above; a desktop has its own).
declare -A NO_FONT=(["void"]=1 ["void-musl"]=1)

test_distro() {
  local distro=$1 out="$OUT/$1"
  compgen -G "$out/*t-code*" >/dev/null || die "No $distro package in $out, run packaging/build.sh $distro"
  info "Testing the $distro package in a fresh ${IMAGE[$distro]}"
  run_in_container "$distro" "$out" bash -c "
    set -euo pipefail
    { ${INSTALL[$distro]//$'\n'/ }; } >/tmp/install.log 2>&1 || { tail -40 /tmp/install.log; exit 1; }
    useradd -m tester
    runuser -u tester -- env VERSION=$VERSION NO_FONT_PACKAGE=${NO_FONT[$distro]:-} bash /src/packaging/smoke-test.sh"
}

test_flatpak() {
  local out="$OUT/flatpak"
  local bundle="$out/t-code-$VERSION-$(flatpak --default-arch).flatpak"
  [[ -f "$bundle" ]] || die "No Flatpak bundle at $bundle, run packaging/build.sh flatpak"
  info "Smoke-testing the Flatpak in an isolated user installation"
  bash "$PACKAGING/flatpak/smoke-test.sh" "$bundle"
}

test_snap() {
  local bundle="$OUT/snap/t-code-ide_${VERSION}_amd64.snap"
  [[ -f "$bundle" ]] || die "No Snap package at $bundle, run packaging/build.sh snap"
  info "Smoke-testing the strictly confined Snap on this Linux host"
  bash "$PACKAGING/snap/test.sh" "$bundle"
}

targets=("$@")
((${#targets[@]})) || targets=(all)
[[ " ${targets[*]} " == *" all "* ]] && targets=("${DISTROS[@]}")
failed=()
for distro in "${targets[@]}"; do
  case "$distro" in
    fedora | arch | aur | debian | void | void-musl) test_distro "$distro" || failed+=("$distro") ;;
    flatpak) test_flatpak || failed+=("flatpak") ;;
    snap) test_snap || failed+=("snap") ;;
    *) die "Unknown target '$distro' (fedora, arch, aur, debian, void, void-musl, flatpak, snap or all)" ;;
  esac
done
((${#failed[@]} == 0)) || die "Failed: ${failed[*]}"
info "All package tests passed: ${targets[*]}"
