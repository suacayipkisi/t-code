#!/usr/bin/env bash
# Removes everything install.sh put in place. Your own files and the repo are not touched.
set -euo pipefail

PREFIX="${PREFIX:-$HOME/.local}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"

for path in "$PREFIX/bin/t-code" "$PREFIX/libexec/t-code" "$PREFIX/share/t-code" \
  "$DATA_HOME/applications/io.github.suacayipkisi.tcode.desktop" \
  "$DATA_HOME/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg" \
  "$DATA_HOME/applications/io.github.tcode.desktop" \
  "$DATA_HOME/icons/hicolor/scalable/apps/io.github.tcode.svg"; do
  if [[ -e "$path" || -L "$path" ]]; then
    rm -rf "$path" && echo "Removed $path"
  fi
done
command -v update-desktop-database >/dev/null &&
  update-desktop-database "$DATA_HOME/applications" 2>/dev/null || true

echo "Done."
