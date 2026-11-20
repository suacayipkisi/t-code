#!/bin/sh
# t-code runs this with the command id ($1). The plugin is C source (the registry takes no
# binaries): it is built with the system's C compiler into t-code's cache, once per version of
# pomodoro.c (its checksum is in the program's name), then the program runs.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dir="${XDG_CACHE_HOME:-$HOME/.cache}/t-code-pomodoro"
sum=$(cksum < "$here/pomodoro.c" | cut -d ' ' -f 1)
program="$dir/pomodoro-$sum"

if [ ! -x "$program" ]; then
  compiler=$(command -v cc || command -v gcc || command -v clang || true)
  if [ -z "$compiler" ]; then
    echo "pomodoro: needs a C compiler (cc, gcc or clang) to build itself once" >&2
    exit 1
  fi
  mkdir -p "$dir"
  # its own temporary name: the start program and a command may build at the same time
  "$compiler" -std=c11 -O2 -o "$program.$$" "$here/pomodoro.c"
  mv -f "$program.$$" "$program"
  for old in "$dir"/pomodoro-*; do
    [ "$old" = "$program" ] || rm -f "$old"
  done
fi
exec "$program" "$@"
