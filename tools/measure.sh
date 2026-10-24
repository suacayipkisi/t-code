#!/bin/bash
# The README's memory measurement: tools/headless.sh with the same 25 C files every time (the first
# 25 src/*/*.c of the v0.3 tag, so releases compare), each tab shown in turn over the socket, then
# proc-report.sh; repeated, as clangd's share varies a lot between runs.
#   tools/measure.sh [-n runs] [-w seconds] [tree]
#     -n: how many runs (default 3)
#     -w: seconds from start to the measurement (default 75: 25 tabs 2 s apart, then clangd settles)
#     tree: a t-code source tree with build/t-code (default: this one); e.g. an older release
#           unpacked with git archive, to measure both on the same machine and day
set -euo pipefail

runs=3 wait=75
while getopts n:w: opt; do
  case $opt in
    n) runs=$OPTARG ;;
    w) wait=$OPTARG ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tree="$(cd "${1:-$root}" && pwd)"
[[ -x "$tree/build/t-code" ]] || make -C "$tree" >/dev/null
mapfile -t files < <(git -C "$root" ls-tree -r --name-only v0.3 src | grep -E '^src/[^/]+/[^/]+\.c$' | LC_ALL=C sort |
  head -25)
[[ ${#files[@]} -eq 25 ]] || { echo "no v0.3 tag here: git fetch --tags" >&2; exit 1; }
for f in "${files[@]}"; do
  [[ -f "$tree/$f" ]] || { echo "$tree has no $f" >&2; exit 1; }
done

out=$(mktemp)
trap 'rm -f "$out"' EXIT
for run in $(seq "$runs"); do
  echo "== run $run: $("$tree/build/t-code" --version)"
  (cd "$tree" && "$root/tools/headless.sh" -w "$wait" -s 5 "${files[@]}") > "$out" 2>&1 &
  report=$!
  pid=
  for _ in $(seq 100); do
    pid=$(sed -n 's/^t-code \([0-9]*\) on Broadway.*/\1/p' "$out" 2>/dev/null)
    [[ -n "$pid" ]] && break
    sleep 0.2
  done
  [[ -n "$pid" ]] || { wait "$report" || true; cat "$out" >&2; exit 1; }
  sleep 5
  for f in "${files[@]}"; do
    TCODE_SOCKET="$XDG_RUNTIME_DIR/t-code-$pid.sock" "$tree/build/t-code" --send "open $tree/$f"
    sleep 2
  done
  wait "$report"
  grep -v '^t-code [0-9]* on Broadway' "$out"
done
