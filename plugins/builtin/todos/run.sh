#!/bin/sh
# t-code runs this with the command id ($1). The plugin is Rust source (the registry takes no
# binaries): it is built once with cargo, offline (it has no dependencies), into t-code's cache,
# and again when its source changes; then the program runs.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
target="${XDG_CACHE_HOME:-$HOME/.cache}/t-code-todos"
program="$target/release/todos"

if command -v cargo >/dev/null 2>&1; then
  CARGO_TARGET_DIR="$target" cargo build --release --offline --locked --quiet \
    --manifest-path "$here/Cargo.toml" >&2
elif [ ! -x "$program" ]; then
  echo "todos: needs Rust's cargo to build itself once (e.g. dnf install cargo, apt install cargo)" >&2
  exit 1
fi
exec "$program" "$@"
