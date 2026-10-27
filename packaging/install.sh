#!/usr/bin/env bash
# Builds t-code and installs it under ~/.local:
#   ~/.local/bin/t-code
#   ~/.local/share/t-code/styles/dark-classic.xml       the editor's colors
#   ~/.local/share/t-code/snippets/*.snippets           the built-in snippets
#   ~/.local/share/applications/io.github.suacayipkisi.tcode.desktop app menu entry
#   ~/.local/share/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg
# Run it again after pulling changes to update the installed copy.
#
# --dev installs the dev variant instead (make DEV=1), to try the working tree next to a t-code
# installed from a package: ~/.local/bin/t-code-dev, ~/.local/share/t-code-dev, the menu entry
# "t-code Dev" (io.github.suacayipkisi.tcode.Devel) and its icon, drawn in blue. It has its own
# settings, session, log and socket, and checks for no updates. For this machine only: no package
# builds it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
DEV=0
[[ "${1:-}" == "--dev" ]] && DEV=1
if ((DEV)); then
  NAME=t-code-dev APP_ID=io.github.suacayipkisi.tcode.Devel MENU_NAME="t-code Dev"
else
  NAME=t-code APP_ID=io.github.suacayipkisi.tcode MENU_NAME=t-code
fi
BIN="$PREFIX/bin/$NAME"
LIBEXEC="$PREFIX/libexec/t-code"
SHARE="$PREFIX/share/$NAME"
APP_DIR="$DATA_HOME/applications"
ICON="$DATA_HOME/icons/hicolor/scalable/apps/$APP_ID.svg"

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
make -C "$ROOT" --no-print-directory DEV="$DEV"

# 3. Programs (a leftover symlink from a manual 'ln -s' setup is replaced by a real copy)
[[ -L "$BIN" ]] && rm "$BIN"
install -Dm755 "$ROOT/build/$NAME" "$BIN"
((DEV)) || rm -rf "$LIBEXEC" # t-code-fm, the terminal file manager before 0.2.1: the file tree is built in now
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
# Before 0.4.1 the application ID was io.github.tcode: drop that entry so the menu shows one t-code.
rm -f "$APP_DIR/io.github.tcode.desktop" "$DATA_HOME/icons/hicolor/scalable/apps/io.github.tcode.svg"
install -d "$(dirname "$ICON")"
if ((DEV)); then # the stable icon has gold braces and a grey t, the dev one blue ones
  sed -e 's/#FFD700/#3794FF/g' -e 's/#CCCCCC/#3794FF/g' "$ROOT/desktop/io.github.suacayipkisi.tcode.svg" > "$ICON"
else
  install -m644 "$ROOT/desktop/io.github.suacayipkisi.tcode.svg" "$ICON"
fi
touch "$DATA_HOME/icons/hicolor" # tells the desktop to rescan its icon cache
mkdir -p "$APP_DIR"
# The dev variant claims no file types: opening a file from the file manager stays with t-code.
((DEV)) && no_mime='/^MimeType=/d' || no_mime=''
sed -e "s|@BIN@|$BIN|" -e "s|^Name=t-code$|Name=$MENU_NAME|" \
  -e "s|io\.github\.suacayipkisi\.tcode$|$APP_ID|" -e "$no_mime" \
  "$ROOT/desktop/io.github.suacayipkisi.tcode.desktop.in" > "$APP_DIR/$APP_ID.desktop"
command -v update-desktop-database >/dev/null && update-desktop-database "$APP_DIR" 2>/dev/null || true
info "Installed desktop entry: $APP_DIR/$APP_ID.desktop"

info "Done. Run '$NAME [dir]' or open '$MENU_NAME' from the app menu."
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) warn "$PREFIX/bin is not in your PATH" ;; esac
