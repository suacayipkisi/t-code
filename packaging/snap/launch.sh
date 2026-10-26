#!/bin/sh
set -eu

# Keep configuration and executable plugins private to this snap, across revisions.
export XDG_CONFIG_HOME="$SNAP_USER_COMMON/.config"
export XDG_DATA_HOME="$SNAP_USER_COMMON/.local/share"
export XDG_CACHE_HOME="$SNAP_USER_COMMON/.cache"
mkdir -p "$XDG_CONFIG_HOME/t-code/plugins" "$XDG_CONFIG_HOME/git" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_GLOBAL="$XDG_CONFIG_HOME/git/config"

# snapd supplies /run/user/<uid>/snap.t-code-ide; do not move IPC into the host runtime.
: "${XDG_RUNTIME_DIR:?snapd must provide XDG_RUNTIME_DIR}"
export PATH="$SNAP/bin:$SNAP/usr/bin:$SNAP/toolchain/usr/bin:$PATH"
export SHELL=/bin/bash
export CC="$SNAP/bin/cc"
export CXX="$SNAP/bin/c++"
export GIT_EXEC_PATH="$SNAP/toolchain/usr/lib/git-core"
export GIT_TEMPLATE_DIR="$SNAP/toolchain/usr/share/git-core/templates"
export GIT_SSL_CAINFO="$SNAP/toolchain/etc/ssl/certs/ca-certificates.crt"
export CURL_CA_BUNDLE="$GIT_SSL_CAINFO"
export SSL_CERT_FILE="$GIT_SSL_CAINFO"
export PYTHONHOME="$SNAP/toolchain/usr"
export PYTHONPATH="$SNAP/toolchain/usr/lib/python3/dist-packages"

triplet=$(cat "$SNAP/usr/lib/t-code/toolchain-triplet")
# Clang/clangd must see the bundled headers, not the base's empty development tree.
export C_INCLUDE_PATH="$SNAP/toolchain/usr/include:$SNAP/toolchain/usr/include/$triplet"
export CPLUS_INCLUDE_PATH="$SNAP/toolchain/usr/include/c++/15:$SNAP/toolchain/usr/include/$triplet/c++/15:$SNAP/toolchain/usr/include/c++/15/backward:${C_INCLUDE_PATH:-}"
export PKG_CONFIG_SYSROOT_DIR="$SNAP/toolchain"
export PKG_CONFIG_LIBDIR="$SNAP/toolchain/usr/lib/$triplet/pkgconfig:$SNAP/toolchain/usr/lib/pkgconfig:$SNAP/toolchain/usr/share/pkgconfig"
# Keep the IDE's GNOME libraries ahead of the toolchain's private dependencies.
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:+$LD_LIBRARY_PATH:}$SNAP/toolchain/usr/lib/$triplet:$SNAP/toolchain/usr/lib"

if [ "${TCODE_SNAP_SMOKE_TEST:-0}" = 1 ]; then
  unset TCODE_SNAP_SMOKE_TEST
  exec "$SNAP/toolchain/usr/bin/python3" "$SNAP/usr/lib/t-code/snap-smoke.py"
fi

exec "$@"
