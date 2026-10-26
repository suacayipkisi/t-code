#!/bin/bash
# What a running t-code costs: its process tree grouped by program, with process and thread
# counts, memory (PSS: shared pages split between the processes using them, so the column adds
# up; RSS counts them in every process) and CPU over a few seconds, plus the check that keeps it
# small (clangd's malloc arenas).
#   tools/proc-report.sh [seconds] [t-code pid]     (default: 5 seconds, the oldest t-code)
set -euo pipefail
export LC_ALL=C

seconds=${1:-5}
root=${2:-$(pgrep -o -x t-code || true)}
[[ -n "$root" ]] || { echo "t-code is not running" >&2; exit 1; }
hz=$(getconf CLK_TCK)

tree() { # the pid and all its descendants
  local pids=("$1") i=0
  while ((i < ${#pids[@]})); do
    pids+=($(pgrep -P "${pids[i]}" || true))
    i=$((i + 1))
  done
  printf '%s\n' "${pids[@]}"
}
ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

mapfile -t pids < <(tree "$root")
declare -A before
for p in "${pids[@]}"; do before[$p]=$(ticks "$p"); done
sleep "$seconds"

declare -A count threads pss rss cpu
for p in "${pids[@]}"; do
  [[ -r /proc/$p/status ]] || continue
  name=$(cat "/proc/$p/comm")
  count[$name]=$(( ${count[$name]:-0} + 1 ))
  threads[$name]=$(( ${threads[$name]:-0} + $(awk '/^Threads:/{print $2}' "/proc/$p/status") ))
  rss[$name]=$(( ${rss[$name]:-0} + $(awk '/^VmRSS:/{print $2}' "/proc/$p/status") ))
  pss[$name]=$(( ${pss[$name]:-0} + $(awk '/^Pss:/{print $2}' "/proc/$p/smaps_rollup" 2>/dev/null || echo 0) ))
  cpu[$name]=$(( ${cpu[$name]:-0} + $(ticks "$p") - ${before[$p]:-0} ))
done

{
  for name in "${!count[@]}"; do
    echo "$name ${count[$name]} ${threads[$name]} ${pss[$name]} ${rss[$name]} ${cpu[$name]}"
  done
} | sort -k4 -n -r | awk -v hz="$hz" -v s="$seconds" '
  BEGIN { printf "%-18s %5s %7s %9s %9s %7s\n", "program", "procs", "threads", "PSS MB", "RSS MB", "CPU %" }
  { printf "%-18s %5d %7d %9.1f %9.1f %7.1f\n", $1, $2, $3, $4 / 1024, $5 / 1024, $6 * 100 / hz / s
    pss += $4; cpu += $6 }
  END { printf "%-18s %22.1f %17.1f   (CPU: %% of one core over %ss)\n", "total", pss / 1024, cpu * 100 / hz / s, s }'

echo
for p in $(pgrep -P "$root" -f '^(/[^ ]*/)?clangd ' || true); do
  if tr '\0' '\n' < "/proc/$p/environ" | grep -q 'arena_max='; then
    echo "clangd $p: malloc arenas limited ($(tr '\0' '\n' < "/proc/$p/environ" | grep -o 'arena_max=[0-9]*'))"
  else
    echo "clangd $p: malloc arenas NOT limited (started by an older t-code?)"
  fi
done
