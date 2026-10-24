#!/usr/bin/env bash
# Installs the packages from packaging/packages/<distro>/ into a fresh container of that distro
# (so the package manager has to pull in every dependency itself) and runs smoke-test.sh there.
# Usage: packaging/test.sh [fedora|arch|debian|void|void-musl|appimage|all]...   (default: all)
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# Installs the package, then what smoke-test.sh needs: a headless X server, a session bus,
# python3 (to talk to the socket) and pgrep. Only this version's package ($VERSION, set in the
# container), when older ones are still in packaging/packages.
declare -A INSTALL=(
  [fedora]='dnf -y install $(ls /out/t-code-$VERSION-[0-9]*.x86_64.rpm) xorg-x11-server-Xvfb dbus-daemon
            python3 procps-ng desktop-file-utils fontconfig util-linux'
  [arch]='pacman -Syu --noconfirm && pacman -U --noconfirm $(ls /out/t-code-$VERSION-[0-9]*-x86_64.pkg.tar.*) &&
          pacman -S --noconfirm --needed xorg-server-xvfb dbus python procps-ng desktop-file-utils'
  [debian]='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y
            $(ls /out/t-code_$VERSION-[0-9]*_amd64.deb) xvfb dbus python3
            procps desktop-file-utils'
  [appimage]='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y
              xvfb dbus python3 procps desktop-file-utils clangd git file binutils'
  # A local repository: xbps installs from repositories only, not from a package file.
  [void]='xbps-install -Syu xbps && xbps-install -Syu && mkdir -p /tmp/repo &&
          cp /out/t-code-${VERSION}_[0-9]*.xbps /tmp/repo/ && xbps-rindex -a /tmp/repo/*.xbps &&
          xbps-install -y --repository=/tmp/repo t-code xorg-server-xvfb dbus python3 procps-ng
          desktop-file-utils fontconfig util-linux dejavu-fonts-ttf'
  [void-musl]='xbps-install -Syu xbps && xbps-install -Syu && mkdir -p /tmp/repo &&
          cp /out/t-code-${VERSION}_[0-9]*.xbps /tmp/repo/ && xbps-rindex -a /tmp/repo/*.xbps &&
          xbps-install -y --repository=/tmp/repo t-code xorg-server-xvfb dbus python3 procps-ng
          desktop-file-utils fontconfig util-linux dejavu-fonts-ttf'
)
# Void has no package for the Cascadia Code font: t-code falls back to the default monospace one
# (a bare container has no font at all, hence DejaVu above; a desktop has its own).
declare -A NO_FONT=([void]=1 [void-musl]=1)

test_distro() {
  local distro=$1 out="$OUT/$1"
  compgen -G "$out/*t-code*" >/dev/null || die "No $distro package in $out, run packaging/build.sh $distro"
  info "Testing the $distro package in a fresh ${IMAGE[$distro]}"
  if [[ "$distro" == "appimage" ]]; then
    run_in_container "$distro" "$out" bash -c "
      set -euo pipefail
      { ${INSTALL[appimage]//$'\n'/ }; } >/tmp/install.log 2>&1 || { tail -40 /tmp/install.log; exit 1; }
      useradd -m tester
      runuser -u tester -- env VERSION=$VERSION bash /src/packaging/appimage/smoke-test.sh /out/t-code-$VERSION-x86_64.AppImage"
  else
    run_in_container "$distro" "$out" bash -c "
      set -euo pipefail
      { ${INSTALL[$distro]//$'\n'/ }; } >/tmp/install.log 2>&1 || { tail -40 /tmp/install.log; exit 1; }
      useradd -m tester
      runuser -u tester -- env VERSION=$VERSION NO_FONT_PACKAGE=${NO_FONT[$distro]:-} bash /src/packaging/smoke-test.sh"
  fi
}

targets=("$@")
((${#targets[@]})) || targets=(all)
[[ " ${targets[*]} " == *" all "* ]] && targets=("${DISTROS[@]}")
failed=()
for distro in "${targets[@]}"; do
  case "$distro" in
    fedora | arch | debian | void | void-musl | appimage) test_distro "$distro" || failed+=("$distro") ;;
    *) die "Unknown target '$distro' (fedora, arch, debian, void, void-musl, appimage or all)" ;;
  esac
done
((${#failed[@]} == 0)) || die "Failed: ${failed[*]}"
info "All package tests passed: ${targets[*]}"
