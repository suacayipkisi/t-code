#!/usr/bin/env bash
# Runs inside the Debian 13 (trixie) container (packaging/build.sh appimage):
# builds the portable AppImage from /out/t-code-$VERSION.tar.gz into /out/t-code-$VERSION-x86_64.AppImage.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

info "Installing build dependencies and tools in container"
apt-get update -qq
apt-get install -y --no-install-recommends \
  build-essential pkgconf libgtk-4-dev libgtksourceview-5-dev libgtksourceview-5-common \
  libvte-2.91-gtk4-dev libjson-glib-dev libmd4c-dev \
  patchelf desktop-file-utils fonts-cascadia-code librsvg2-common \
  adwaita-icon-theme hicolor-icon-theme \
  curl file python3 ca-certificates squashfs-tools

work="/tmp/build"
rm -rf "$work" && mkdir -p "$work"

info "Extracting source archive"
tar -C "$work" -xzf "/out/t-code-$VERSION.tar.gz"
src="$work/t-code-$VERSION"

info "Compiling t-code with RPATH"
(
  cd "$src"
  make clean
  make -j"$(nproc)" CFLAGS="-O2 -g -Wall -Wextra -std=c23"
)

info "Creating AppDir filesystem"
appdir="/tmp/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" \
         "$appdir/usr/lib" \
         "$appdir/usr/share/t-code/styles" \
         "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/scalable/apps" \
         "$appdir/usr/share/glib-2.0/schemas" \
         "$appdir/usr/share/gtksourceview-5" \
         "$appdir/usr/share/fonts/cascadia-code"

# Copy binary & assets
cp "$src/build/t-code" "$appdir/usr/bin/"
cp "$src/styles/dark-classic.xml" "$appdir/usr/share/t-code/styles/"
cp "$src/desktop/io.github.tcode.svg" "$appdir/usr/share/icons/hicolor/scalable/apps/"
sed 's|@BIN@|t-code|' "$src/desktop/io.github.tcode.desktop.in" > "$appdir/usr/share/applications/io.github.tcode.desktop"
desktop-file-validate "$appdir/usr/share/applications/io.github.tcode.desktop"

# Root level AppDir links & AppRun
ln -sf usr/share/applications/io.github.tcode.desktop "$appdir/io.github.tcode.desktop"
ln -sf usr/share/icons/hicolor/scalable/apps/io.github.tcode.svg "$appdir/io.github.tcode.svg"
ln -sf io.github.tcode.svg "$appdir/.DirIcon"
cp /src/packaging/appimage/AppRun "$appdir/AppRun"
chmod +x "$appdir/AppRun"

# GSettings schemas: copy GTK4 and GtkSourceView schemas and compile
info "Bundling and compiling GSettings schemas"
cp /usr/share/glib-2.0/schemas/*gtk* "$appdir/usr/share/glib-2.0/schemas/" 2>/dev/null || true
cp /usr/share/glib-2.0/schemas/*gschema* "$appdir/usr/share/glib-2.0/schemas/" 2>/dev/null || true
glib-compile-schemas "$appdir/usr/share/glib-2.0/schemas/"

# GtkSourceView-5 language specs and style schemes
info "Bundling GtkSourceView-5 language specifications and styles"
cp -r /usr/share/gtksourceview-5/* "$appdir/usr/share/gtksourceview-5/"

# Icons: ensure base Adwaita and hicolor scalable icons exist
info "Bundling base UI icons"
mkdir -p "$appdir/usr/share/icons"
cp -r /usr/share/icons/Adwaita "$appdir/usr/share/icons/" 2>/dev/null || true
cp -r /usr/share/icons/hicolor "$appdir/usr/share/icons/" 2>/dev/null || true

# Cascadia Code font
info "Bundling Cascadia Code font"
cp -r /usr/share/fonts/truetype/cascadia-code/* "$appdir/usr/share/fonts/cascadia-code/" 2>/dev/null || true

# Shared libraries and RPATH patching
info "Resolving and bundling shared libraries with subprocess isolation"
python3 /src/packaging/appimage/bundle.py "$appdir"

# Download and execute appimagetool
info "Packaging AppDir with appimagetool"
tool_dir="/tmp/appimagetool"
mkdir -p "$tool_dir"
if [[ ! -x "$tool_dir/squashfs-root/AppRun" ]]; then
  info "Downloading appimagetool"
  curl -sSL -o "$tool_dir/appimagetool.AppImage" \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
  chmod +x "$tool_dir/appimagetool.AppImage"
  (cd "$tool_dir" && ./appimagetool.AppImage --appimage-extract >/dev/null)
fi

appimage_out="/out/t-code-$VERSION-x86_64.AppImage"
rm -f "$appimage_out"
ARCH=x86_64 "$tool_dir/squashfs-root/AppRun" --no-appstream "$appdir" "$appimage_out"
chmod +x "$appimage_out"

info "AppImage generated successfully: $appimage_out"
[[ -z "${HOST_OWNER:-}" ]] || chown -R "$HOST_OWNER" /out
