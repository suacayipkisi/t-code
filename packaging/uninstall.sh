#!/usr/bin/env bash
# Removes everything install.sh put in place. Your own files and the repo are not touched.
set -euo pipefail

# --dev removes the dev variant (install.sh --dev) instead; its settings in ~/.config/t-code-dev stay.
PREFIX="${PREFIX:-$HOME/.local}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"

if [[ "${1:-}" == "--dev" ]]; then
  paths=("$PREFIX/bin/t-code-dev" "$PREFIX/share/t-code-dev"
    "$DATA_HOME/applications/io.github.suacayipkisi.tcode.Devel.desktop"
    "$DATA_HOME/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.Devel.svg")
else
  paths=("$PREFIX/bin/t-code" "$PREFIX/libexec/t-code" "$PREFIX/share/t-code"
    "$DATA_HOME/applications/io.github.suacayipkisi.tcode.desktop"
    "$DATA_HOME/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg"
    "$DATA_HOME/applications/io.github.tcode.desktop"
    "$DATA_HOME/icons/hicolor/scalable/apps/io.github.tcode.svg")
fi
for path in "${paths[@]}"; do
  if [[ -e "$path" || -L "$path" ]]; then
    rm -rf "$path" && echo "Removed $path"
  fi
done
command -v update-desktop-database >/dev/null &&
  update-desktop-database "$DATA_HOME/applications" 2>/dev/null || true

echo "Done."
