#!/usr/bin/env bash
# Install and launch a bundle without touching the calling user's Flatpak installation or settings.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${1:-}" == --session ]]; then
  (($# == 2)) || exit 2
  bundle=$2
  app=io.github.suacayipkisi.tcode
  version="$(jq -er '."x-tcode-version"' "$here/io.github.suacayipkisi.tcode.json")"
  flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
  flatpak --user install --assumeyes --noninteractive --bundle "$bundle"
  desktop="$FLATPAK_USER_DIR/exports/share/applications/$app.desktop"
  desktop-file-validate "$desktop"
  grep -q -- '--file-forwarding' "$desktop"
  grep -q -- '@@ %F @@' "$desktop"
  flatpak run --user --command=python3 \
    --env=GDK_BACKEND=x11 --env=GSK_RENDERER=cairo --env=GTK_A11Y=none \
    "$app" - "$version" < "$here/smoke-in-sandbox.py"
  exit
fi
if (($# != 1)); then
  echo "Usage: bash packaging/flatpak/smoke-test.sh /absolute/path/t-code-0.4.1-ARCH.flatpak" >&2
  exit 2
fi
if ((EUID == 0)); then
  echo "Run the Flatpak smoke test as an unprivileged user (the container image provides one)." >&2
  exit 1
fi
for command in flatpak jq desktop-file-validate dbus-run-session xvfb-run xauth; do
  command -v "$command" >/dev/null || { echo "Missing smoke prerequisite: $command" >&2; exit 1; }
done
bundle="$(realpath "$1")"
[[ -f "$bundle" ]] || { echo "Bundle not found: $bundle" >&2; exit 1; }
work="$(mktemp -d)"
cleanup() {
  local status=$?
  if ((status != 0)) && [[ -f "$work/xvfb.log" ]]; then
    cat "$work/xvfb.log" >&2
  fi
  rm -rf "$work"
}
trap cleanup EXIT
export HOME="$work/home" XDG_DATA_HOME="$work/data" XDG_CONFIG_HOME="$work/config"
export XDG_CACHE_HOME="$work/cache" XDG_STATE_HOME="$work/state" XDG_RUNTIME_DIR="$work/runtime"
export FLATPAK_USER_DIR="$work/flatpak"
mkdir -p "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
mkdir -m 700 "$XDG_RUNTIME_DIR"
unset WAYLAND_DISPLAY WAYLAND_SOCKET

dbus-run-session -- xvfb-run -a -e "$work/xvfb.log" -s '-screen 0 1600x1000x24' \
  bash "$here/smoke-test.sh" --session "$bundle"
