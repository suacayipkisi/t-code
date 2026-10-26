#!/usr/bin/env bash
# Builds t-code and installs it under ~/.local:
#   ~/.local/bin/t-code
#   ~/.local/share/t-code/styles/dark-classic.xml       the editor's colors
#   ~/.local/share/t-code/snippets/*.snippets           the built-in snippets
#   ~/.local/share/applications/io.github.tcode.desktop app menu entry
#   ~/.local/share/icons/hicolor/scalable/apps/io.github.tcode.svg
# Run it again after pulling changes to update the installed copy.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
BIN="$PREFIX/bin/t-code"
LIBEXEC="$PREFIX/libexec/t-code"
SHARE="$PREFIX/share/t-code"
APP_DIR="$DATA_HOME/applications"
ICON="$DATA_HOME/icons/hicolor/scalable/apps/io.github.tcode.svg"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*"; }

# 1. Dependencies
missing=()
for cmd in gcc make pkg-config clangd; do
  command -v "$cmd" >/dev/null || missing+=("$cmd")
done
if ((${#missing[@]})); then
  warn "Missing required tools: ${missing[*]}"
  warn "Fedora: sudo dnf install gcc make pkgconf clang-tools-extra"
  exit 1
fi
if ! pkg-config --exists gtk4 gtksourceview-5 json-glib-1.0 vte-2.91-gtk4 md4c; then
  warn "GTK4 / GtkSourceView / json-glib / VTE / md4c development files are missing"
  warn "Fedora: sudo dnf install gtk4-devel gtksourceview5-devel json-glib-devel vte291-gtk4-devel md4c-devel"
  exit 1
fi
[[ "$(fc-match -f '%{family}' 'Cascadia Code' 2>/dev/null)" == *"Cascadia Code"* ]] ||
  warn "Font 'Cascadia Code' not found (Fedora: sudo dnf install cascadia-code-fonts)"

# 2. Build
info "Building..."
make -C "$ROOT" --no-print-directory

# 3. Programs (a leftover symlink from a manual 'ln -s' setup is replaced by a real copy)
[[ -L "$BIN" ]] && rm "$BIN"
install -Dm755 "$ROOT/build/t-code" "$BIN"
rm -rf "$LIBEXEC" # t-code-fm, the terminal file manager before 0.2.1: the file tree is built in now
info "Installed $BIN"

# 4. The editor's colors. What the micro-based t-code (0.1.x) installed is not used any more.
install -Dm644 "$ROOT/styles/dark-classic.xml" "$SHARE/styles/dark-classic.xml"
rm -f "$SHARE/styles/dark-modern.xml" # its name before 0.3
rm -rf "${SHARE:?}/micro"
info "Installed the editor's colors in $SHARE/styles"
install -d "$SHARE/snippets"
install -m644 "$ROOT"/snippets/*.snippets "$SHARE/snippets/"
info "Installed the snippets in $SHARE/snippets"

# 5. App menu entry and icon
install -Dm644 "$ROOT/desktop/io.github.tcode.svg" "$ICON"
touch "$DATA_HOME/icons/hicolor" # tells the desktop to rescan its icon cache
mkdir -p "$APP_DIR"
sed "s|@BIN@|$BIN|" "$ROOT/desktop/io.github.tcode.desktop.in" > "$APP_DIR/io.github.tcode.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$APP_DIR" 2>/dev/null || true
info "Installed desktop entry: $APP_DIR/io.github.tcode.desktop"

info "Done. Run 't-code [dir]' or open 't-code' from the app menu."
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) warn "$PREFIX/bin is not in your PATH" ;; esac
