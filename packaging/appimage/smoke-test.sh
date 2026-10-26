#!/usr/bin/env bash
# Comprehensive test suite for t-code AppImage:
# 1. Package integrity & version checks
# 2. Bundled assets, schemas, language-specs, fonts validation
# 3. Host library exclusion check (libc, libGL, libdrm, etc.)
# 4. RPATH & subprocess environment isolation check
# 5. Headless GUI test with Xvfb, DBus, clangd, markdown preview, socket control, clean quit, no GTK criticals
# 6. Extracted AppDir (AppRun) execution
set -uo pipefail

failures=0
pass() { printf '  \033[1;32mok\033[0m   %s\n' "$1"; }
fail() { printf '  \033[1;31mFAIL\033[0m %s\n' "$1"; failures=$((failures + 1)); }
check() { # <description> <command...>
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VERSION="${VERSION:-$(sed -n 's/^VERSION *:= *//p' "$ROOT/Makefile")}"
APPIMAGE="${1:-$ROOT/packaging/packages/appimage/t-code-$VERSION-x86_64.AppImage}"
[[ -f "$APPIMAGE" ]] || { echo "AppImage not found at $APPIMAGE"; exit 1; }

echo "=== 1. AppImage Integrity and Version ==="
check "AppImage binary is executable" test -x "$APPIMAGE"
check "file reports ELF executable" sh -c "file '$APPIMAGE' | grep -q 'ELF'"
check "AppImage --appimage-extract-and-run --version matches $VERSION" \
  sh -c "test \"\$('$APPIMAGE' --appimage-extract-and-run --version)\" = 't-code $VERSION'"

echo "=== 2. Extraction and Asset Verification ==="
EXTRACT_DIR="$(mktemp -d /tmp/appimage-test-XXXXXX)"
trap 'rm -rf "$EXTRACT_DIR" /tmp/t-code-test.log' EXIT

(cd "$EXTRACT_DIR" && "$APPIMAGE" --appimage-extract >/dev/null)
APPDIR="$EXTRACT_DIR/squashfs-root"

check "AppRun exists and is executable" test -x "$APPDIR/AppRun"
check "t-code binary installed in usr/bin" test -x "$APPDIR/usr/bin/t-code"
check "Dark Classic style scheme bundled" test -f "$APPDIR/usr/share/t-code/styles/dark-classic.xml"
check "Desktop file valid" desktop-file-validate "$APPDIR/usr/share/applications/io.github.suacayipkisi.tcode.desktop"
check "SVG icon bundled" test -f "$APPDIR/usr/share/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg"
check "GSettings gschemas.compiled exists" test -s "$APPDIR/usr/share/glib-2.0/schemas/gschemas.compiled"
check "GtkSourceView-5 language-specs bundled" test -d "$APPDIR/usr/share/gtksourceview-5/language-specs"
check "GtkSourceView language specs available (bundled or embedded)" \
  sh -c "test -f '$APPDIR/usr/share/gtksourceview-5/language-specs/c.lang' || gresource list '$APPDIR'/usr/lib/libgtksourceview-5.so* 2>/dev/null | grep -q 'language-specs/c.lang' || strings '$APPDIR'/usr/lib/libgtksourceview-5.so* 2>/dev/null | grep -q 'language-specs/c.lang'"
check "Cascadia Code font bundled" sh -c "ls '$APPDIR/usr/share/fonts/cascadia-code/'* >/dev/null 2>&1"
check "GdkPixbuf loaders bundled" sh -c "ls '$APPDIR/usr/lib/gdk-pixbuf-2.0/loaders/'*.so >/dev/null 2>&1"

echo "=== 3. Host Driver and Libc Exclusion Checks ==="
# Ensuring libc, libm, libGL, libdrm etc are NOT bundled
check "libc.so.6 is NOT bundled in usr/lib" sh -c "! ls '$APPDIR'/usr/lib/libc.so* 2>/dev/null"
check "libpthread.so is NOT bundled in usr/lib" sh -c "! ls '$APPDIR'/usr/lib/libpthread.so* 2>/dev/null"
check "libGL.so is NOT bundled in usr/lib" sh -c "! ls '$APPDIR'/usr/lib/libGL.so* 2>/dev/null"
check "libEGL.so is NOT bundled in usr/lib" sh -c "! ls '$APPDIR'/usr/lib/libEGL.so* 2>/dev/null"
check "libdrm.so is NOT bundled in usr/lib" sh -c "! ls '$APPDIR'/usr/lib/libdrm.so* 2>/dev/null"
# The host's Mesa loads its own Wayland / X11 client libraries, and the font stack reads the
# host's /etc/fonts: bundled older copies broke EGL on Wayland and flooded fontconfig warnings.
for lib in libwayland-client libwayland-egl libwayland-cursor libX11 libX11-xcb libxcb libfontconfig \
  libfreetype libharfbuzz; do
  check "$lib.so is NOT bundled in usr/lib" sh -c "! ls '$APPDIR'/usr/lib/$lib.so* 2>/dev/null"
done
check "librsvg (the SVG icon loader's library) is bundled" sh -c "ls '$APPDIR'/usr/lib/librsvg-2.so* >/dev/null 2>&1"
check "the SVG loader finds librsvg inside the AppDir" \
  sh -c "ldd '$APPDIR/usr/lib/gdk-pixbuf-2.0/loaders/libpixbufloader_svg.so' | grep librsvg | grep -q '$APPDIR'"

echo "=== 4. RPATH and Subprocess Isolation Checks ==="
check "t-code binary has RUNPATH with \$ORIGIN/../lib" \
  sh -c "objdump -p '$APPDIR/usr/bin/t-code' | grep -E 'RUNPATH|RPATH' | grep -q '\\\$ORIGIN/../lib'"

check "bundled libgtk-4.so has RUNPATH with \$ORIGIN" \
  sh -c "objdump -p '$APPDIR'/usr/lib/libgtk-4.so* | grep -E 'RUNPATH|RPATH' | grep -q '\\\$ORIGIN'"

# Verify that external tools (gcc, git, python3) run cleanly inside AppRun environment
check "system gcc runs cleanly in AppRun environment" \
  env -i PATH="$PATH" HOME="/tmp" "$APPDIR/AppRun" --version

echo "=== 5. Running t-code AppImage under Headless Display ==="
export XDG_RUNTIME_DIR="/tmp/runtime-appimage-${USER:-test}"
mkdir -p -m 700 "$XDG_RUNTIME_DIR"

disp_pid=0
if command -v Xvfb >/dev/null; then
  export DISPLAY=:98 GDK_BACKEND=x11 GSK_RENDERER=cairo
  Xvfb :98 -screen 0 1600x1000x24 >/dev/null 2>&1 &
  disp_pid=$!
elif command -v gtk4-broadwayd >/dev/null; then
  export BROADWAY_DISPLAY=:55 GDK_BACKEND=broadway GSK_RENDERER=cairo
  gtk4-broadwayd :55 >/dev/null 2>&1 &
  disp_pid=$!
else
  echo "Neither Xvfb nor gtk4-broadwayd found, cannot test headless display."
  exit 1
fi
sleep 2

project="/tmp/appimage-project-${USER:-test}"
rm -rf "$project" && mkdir -p "$project"
printf '#include <stdio.h>\n\nint main(void)\n{\n    printf("AppImage test\\n");\n    return 0;\n}\n' > "$project/main.c"
for f in a b c d; do printf 'int %s(void) { return 0; }\n' "$f" > "$project/$f.c"; done
git -C "$project" init -q

(cd "$project" && exec dbus-run-session -- "$APPIMAGE" --appimage-extract-and-run "$project"/{a,b,c,d}.c "$project/main.c") > /tmp/t-code-test.log 2>&1 &
app_pid=$!

# Wait for clangd to start
for _ in $(seq 30); do pgrep -u "$USER" -f '^(/[^ ]*/)?clangd ' >/dev/null && break; sleep 1; done

tcode_pid=$(pgrep -u "$USER" -x t-code | head -1)
check "t-code AppImage process is running" test -n "$tcode_pid"
check "clangd started by AppImage" pgrep -u "$USER" -f '^(/[^ ]*/)?clangd '
check "one clangd for all five editors" test "$(pgrep -u "$USER" -fc '^(/[^ ]*/)?clangd ')" -eq 1
check "clangd runs with 2 malloc arenas" \
  sh -c "tr '\\0' '\\n' < /proc/\$(pgrep -u $USER -f '^(/[^ ]*/)?clangd ' | head -1)/environ | grep -q arena_max=2"
check "control socket created" sh -c "ls $XDG_RUNTIME_DIR/t-code-*.sock"

# Markdown preview test
printf '# Notes\n\nAppImage markdown preview test\n' > "$project/README.md"
python3 -I -c 'import socket, sys; s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.send(sys.argv[2].encode())' \
  "$XDG_RUNTIME_DIR/t-code-$tcode_pid.sock" "preview $project/README.md"$'\n' 2>/dev/null
sleep 1
check "Markdown preview opens in-process" kill -0 "$tcode_pid"

# Terminate cleanly
kill -TERM "${tcode_pid:-0}" 2>/dev/null
for _ in $(seq 10); do kill -0 "$app_pid" 2>/dev/null || break; sleep 1; done

check "t-code AppImage quits cleanly on SIGTERM" sh -c "! pgrep -u $USER -x t-code"
check "control socket removed on quit" sh -c "! ls $XDG_RUNTIME_DIR/t-code-*.sock 2>/dev/null"
check "no GTK criticals or segfaults in log" sh -c "! grep -E 'CRITICAL|segfault|Segmentation' /tmp/t-code-test.log"

[[ "$disp_pid" -gt 0 ]] && kill "$disp_pid" 2>/dev/null || true

echo "=== 6. Extracted AppDir (AppRun) Execution ==="
check "extracted AppRun starts and responds to --version" "$APPDIR/AppRun" --version

# The AppImage mounts at a new path every run: each AppDir location gets a loaders cache of its own,
# pointing at its own loaders (one shared cache kept an earlier mount's paths: no icons from the
# second start on).
OTHER="$EXTRACT_DIR/moved-root"
cp -a "$APPDIR" "$OTHER"
"$APPDIR/AppRun" --version >/dev/null 2>&1
"$OTHER/AppRun" --version >/dev/null 2>&1
caches="$XDG_RUNTIME_DIR/t-code-$(id -u)"
check "a loaders cache points at the first AppDir's loaders" \
  sh -c "grep -l '\"$APPDIR/usr/lib/gdk-pixbuf-2.0/loaders/libpixbufloader_svg.so\"' '$caches'/gdk-pixbuf-loaders-*.cache"
check "another one points at the moved AppDir's loaders" \
  sh -c "grep -l '\"$OTHER/usr/lib/gdk-pixbuf-2.0/loaders/libpixbufloader_svg.so\"' '$caches'/gdk-pixbuf-loaders-*.cache"
rm -rf "$OTHER"
"$APPDIR/AppRun" --version >/dev/null 2>&1
check "the gone AppDir's cache is removed at the next start" \
  sh -c "! grep -l '$OTHER/' '$caches'/gdk-pixbuf-loaders-*.cache"

if ((failures)); then
  echo "--- t-code log ---"
  cat /tmp/t-code-test.log 2>/dev/null || true
  echo "$failures check(s) failed"
  exit 1
fi

echo "All AppImage tests passed successfully!"
