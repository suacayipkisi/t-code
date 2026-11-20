#!/bin/sh
# t-code runs this with the command id ($1). The plugin is Go source (the registry takes no
# binaries): it is built with the system's go into t-code's cache, once per version of its source
# (the checksum is in the program's name), offline (no dependencies, no toolchain download).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dir="${XDG_CACHE_HOME:-$HOME/.cache}/t-code-format-on-save"
sum=$(cat "$here"/*.go "$here/go.mod" | cksum | cut -d ' ' -f 1)
program="$dir/format-on-save-$sum"

if [ ! -x "$program" ]; then
  if ! command -v go >/dev/null 2>&1; then
    echo "format-on-save: needs Go (dnf install golang, apt install golang-go) to build itself once" >&2
    exit 1
  fi
  mkdir -p "$dir"
  # its own temporary name: the start program and a command may build at the same time
  (cd "$here" && GOTOOLCHAIN=local GOPROXY=off GOFLAGS=-mod=mod CGO_ENABLED=0 \
    go build -trimpath -o "$program.$$" .)
  mv -f "$program.$$" "$program"
  for old in "$dir"/format-on-save-*; do
    [ "$old" = "$program" ] || rm -f "$old"
  done
fi
exec "$program" "$@"
