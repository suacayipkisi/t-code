#!/usr/bin/env python3
"""
bundle.py: Resolves and copies libraries and assets into AppDir for t-code AppImage.
Ensures RPATH isolation and excludes what has to match the host: libc, the graphics drivers and
the libraries they load themselves (Wayland, X11 / xcb), and the font stack that reads the host's
font configuration.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Libraries that MUST come from the host OS (drivers, low-level display/kernel/libc)
EXCLUDE_PREFIXES = (
    # C library & low-level runtime
    "libc.so",
    "libm.so",
    "libpthread.so",
    "libdl.so",
    "librt.so",
    "libresolv.so",
    "libutil.so",
    "ld-linux",
    # Graphics hardware drivers & Mesa (must match host GPU & kernel drivers)
    "libGL.so",
    "libEGL.so",
    "libGLX.so",
    "libGLdispatch.so",
    "libOpenGL.so",
    "libdrm.so",
    "libgbm.so",
    # Display client libraries: the host's Mesa EGL / GLX load the host's copies; a bundled older
    # libwayland-client next to them made EGL fail (GTK fell back to Cairo, or crashed on a real GPU)
    "libwayland-client.so",
    "libwayland-cursor.so",
    "libwayland-egl.so",
    "libwayland-server.so",
    "libX11.so",
    "libX11-xcb.so",
    "libxcb.so",
    # Font stack: it reads the host's /etc/fonts and font caches, which a bundled older fontconfig
    # does not understand (dozens of "invalid attribute" warnings at every start)
    "libfontconfig.so",
    "libfreetype.so",
    "libharfbuzz.so",
    # Audio
    "libasound.so",
)

def should_exclude(soname: str) -> bool:
    basename = os.path.basename(soname)
    for prefix in EXCLUDE_PREFIXES:
        if basename.startswith(prefix):
            return True
    return False

def get_ldd_deps(binary_path: str) -> dict[str, str]:
    """Runs ldd on a binary and returns a map of soname -> realpath."""
    deps = {}
    try:
        out = subprocess.check_output(["ldd", binary_path], text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        return deps

    for line in out.splitlines():
        line = line.strip()
        # Format: libfoo.so.1 => /path/to/libfoo.so.1 (0x...)
        # or /lib64/ld-linux-x86-64.so.2 (0x...)
        if "=>" in line:
            parts = line.split("=>")
            soname = parts[0].strip()
            rest = parts[1].strip()
            target_path = rest.split()[0] if rest else ""
            if target_path and os.path.isfile(target_path):
                deps[soname] = os.path.realpath(target_path)
        else:
            parts = line.split()
            if parts and os.path.isabs(parts[0]) and os.path.isfile(parts[0]):
                deps[os.path.basename(parts[0])] = os.path.realpath(parts[0])
    return deps

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <AppDir>")
        sys.exit(1)

    appdir = Path(sys.argv[1]).resolve()
    tcode_bin = appdir / "usr" / "bin" / "t-code"
    lib_dir = appdir / "usr" / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)

    if not tcode_bin.is_file():
        print(f"Error: {tcode_bin} not found!")
        sys.exit(1)

    # 1. Gather all transitive dependencies
    to_inspect = [str(tcode_bin)]
    collected_libs: dict[str, str] = {}  # soname -> source path

    # Also include the gdk-pixbuf loaders and what each needs (librsvg for the SVG one, which
    # draws the icons): all of them, not only the first one found
    pixbuf_loader_dir = None
    for p in [
        Path("/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0"),
        Path("/usr/lib64/gdk-pixbuf-2.0"),
        Path("/usr/lib/gdk-pixbuf-2.0"),
    ]:
        loaders = sorted(p.glob("**/loaders/*.so")) if p.exists() else []
        if loaders:
            to_inspect.extend(str(loader) for loader in loaders)
            pixbuf_loader_dir = loaders[0].parent
            break

    visited = set()
    while to_inspect:
        curr = to_inspect.pop()
        if curr in visited:
            continue
        visited.add(curr)

        deps = get_ldd_deps(curr)
        for soname, src_path in deps.items():
            if should_exclude(soname):
                continue
            if soname not in collected_libs:
                collected_libs[soname] = src_path
                to_inspect.append(src_path)

    print(f"Collected {len(collected_libs)} non-host libraries to bundle.")

    # 2. Copy libraries into AppDir/usr/lib/
    for soname, src_path in collected_libs.items():
        dst_path = lib_dir / soname
        if not dst_path.exists():
            try:
                shutil.copy2(src_path, dst_path)
                dst_path.chmod(0o755)
            except Exception as e:
                print(f"Warning: Failed to copy {src_path} -> {dst_path}: {e}")

    # 3. Handle GdkPixbuf loaders & gdk-pixbuf-query-loaders
    appdir_loaders = lib_dir / "gdk-pixbuf-2.0" / "loaders"
    appdir_loaders.mkdir(parents=True, exist_ok=True)

    query_loaders_bin = None
    for q in [
        "/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders",
        "/usr/bin/gdk-pixbuf-query-loaders",
        "/usr/lib64/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders",
    ]:
        if os.path.isfile(q):
            query_loaders_bin = q
            break

    if pixbuf_loader_dir and pixbuf_loader_dir.exists():
        for loader in pixbuf_loader_dir.glob("*.so"):
            dst = appdir_loaders / loader.name
            shutil.copy2(loader, dst)
            dst.chmod(0o755)

    if query_loaders_bin:
        shutil.copy2(query_loaders_bin, lib_dir / "gdk-pixbuf-2.0" / "gdk-pixbuf-query-loaders")
        (lib_dir / "gdk-pixbuf-2.0" / "gdk-pixbuf-query-loaders").chmod(0o755)

    # 4. Copy X11 locale files if present
    x11_locale = Path("/usr/share/X11/locale")
    if x11_locale.exists():
        dst_locale = appdir / "usr" / "share" / "X11" / "locale"
        dst_locale.parent.mkdir(parents=True, exist_ok=True)
        if not dst_locale.exists():
            shutil.copytree(x11_locale, dst_locale, symlinks=True)

    # 5. Patch RPATH using patchelf
    print("Setting RPATH on t-code binary and bundled libraries...")
    # t-code binary looks in $ORIGIN/../lib
    subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/../lib", str(tcode_bin)], check=True)

    # Bundled .so files in usr/lib look in $ORIGIN
    for so_file in lib_dir.glob("*.so*"):
        if so_file.is_file() and not so_file.is_symlink():
            try:
                subprocess.run(
                    ["patchelf", "--set-rpath", "$ORIGIN", str(so_file)],
                    check=True,
                    stderr=subprocess.DEVNULL,
                )
            except Exception as e:
                print(f"Note: patchelf skipped {so_file.name}: {e}")

    # Loaders look in $ORIGIN/../.. (usr/lib)
    for loader in appdir_loaders.glob("*.so"):
        if loader.is_file() and not loader.is_symlink():
            try:
                subprocess.run(
                    ["patchelf", "--set-rpath", "$ORIGIN/../..:$ORIGIN", str(loader)],
                    check=True,
                    stderr=subprocess.DEVNULL,
                )
            except Exception:
                pass

    print("Bundling completed successfully.")

if __name__ == "__main__":
    main()
