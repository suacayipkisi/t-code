#!/bin/sh
# t-code runs this with the command id ($1). The plugin is C++ source (the registry takes no
# binaries): it is built with the system's C++ compiler into t-code's cache, once per version of
# spell.cpp (its checksum is in the program's name), then the program runs.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dir="${XDG_CACHE_HOME:-$HOME/.cache}/t-code-spell"
sum=$(cksum < "$here/spell.cpp" | cut -d ' ' -f 1)
program="$dir/spell-$sum"

if [ ! -x "$program" ]; then
  compiler=$(command -v c++ || command -v g++ || command -v clang++ || true)
  if [ -z "$compiler" ]; then
    echo "spell: needs a C++ compiler (g++ or clang++) to build itself once" >&2
    exit 1
  fi
  mkdir -p "$dir"
  # its own temporary name: the start program and a command may build at the same time
  "$compiler" -std=c++17 -O2 -o "$program.$$" "$here/spell.cpp"
  mv -f "$program.$$" "$program"
  for old in "$dir"/spell-*; do
    [ "$old" = "$program" ] || rm -f "$old"
  done
fi
exec "$program" "$@"
