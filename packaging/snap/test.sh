#!/usr/bin/env bash
# Usage: bash packaging/snap/test.sh /absolute/path/to/t-code_0.3.1_amd64.snap
# This installs packages and intentionally leaves them on a disposable CI host.
set -euo pipefail

[[ $# == 1 && -f "$1" ]] || { echo "Usage: $0 <snap-file>" >&2; exit 2; }
artifact="$(realpath "$1")"
[[ "$(id -u)" != 0 ]] || { echo 'Run as a normal desktop user with passwordless sudo' >&2; exit 1; }
[[ "$(snap debug confinement)" == strict ]] || {
  echo 'Smoke requires snapd with full AppArmor/seccomp confinement, not an ordinary container' >&2
  exit 1
}
if snap list t-code >/dev/null 2>&1; then
  echo 'Use a clean CI host: t-code is already installed' >&2
  exit 1
fi
for provider in core26 gnome-core26 mesa-2604 gtk-common-themes; do
  if ! snap list "$provider" >/dev/null 2>&1; then
    sudo -n snap install "$provider"
  fi
done
sudo -n snap install --dangerous "$artifact"
sudo -n snap connect t-code:gnome-core26 gnome-core26:gnome-core26
sudo -n snap connect t-code:gpu-2604 mesa-2604:gpu-2604
for themes in gtk-3-themes icon-themes sound-themes; do
  sudo -n snap connect "t-code:$themes" "gtk-common-themes:$themes"
done
sudo -n snap connect t-code:home
sudo -n snap connect t-code:network
# Snap needs the systemd user manager to track confined app processes. CI's runner account is a
# service account, not a PAM login, so create the same lingering session a normal login provides.
uid="$(id -u)"
runtime_dir="/run/user/$uid"
sudo -n loginctl enable-linger "$(id -un)"
sudo -n systemctl start "user@$uid.service"
[[ -S "$runtime_dir/bus" ]] || {
  echo "No systemd user bus at $runtime_dir/bus after starting user@$uid.service" >&2
  exit 1
}
export XDG_RUNTIME_DIR="$runtime_dir"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime_dir/bus"
# Exercise the normal app entry point, command chain and security label, not --shell/devmode.
run=(env TCODE_SNAP_SMOKE_TEST=1 GSK_RENDERER=cairo snap run t-code)
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  run=(xvfb-run -a -s '-screen 0 1600x1000x24' env GDK_BACKEND=x11 "${run[@]}")
fi
"${run[@]}"
snap list t-code core26 gnome-core26 mesa-2604 gtk-common-themes
snap connections t-code
