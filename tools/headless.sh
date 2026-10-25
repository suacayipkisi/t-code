#!/bin/bash
# Runs build/t-code without a visible window (GTK's Broadway backend), with its own D-Bus session
# and a temporary config dir (the real session.ini is never touched), waits, prints
# tools/proc-report.sh for it, then quits it with SIGTERM.
#   tools/headless.sh [-w seconds] [-s seconds] [-c key=value]... [t-code args...]
#     -w: wait before measuring (default 8: clangd and its index come up)
#     -s: measure CPU over this many seconds (default 5)
#     -c: a setting in session.ini's [settings] (src/project/session.c), like -c minimap=false
# Example: tools/headless.sh src/*/*.c      (opens every file: clangd gets only the tab shown)
set -euo pipefail

wait=8 sample=5 settings=()
while getopts w:s:c: opt; do
  case $opt in
    w) wait=$OPTARG ;;
    s) sample=$OPTARG ;;
    c) settings+=("$OPTARG") ;;
    *) exit 2 ;;
  esac
done
shift $((OPTIND - 1))

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -x "$root/build/t-code" ]] || make -C "$root" >/dev/null
command -v gtk4-broadwayd >/dev/null || { echo "gtk4-broadwayd is not installed" >&2; exit 1; }

work=$(mktemp -d)
mkdir -p "$work/config/t-code"
printf '[settings]\n' > "$work/config/t-code/session.ini"
printf '%s\n' "${settings[@]}" >> "$work/config/t-code/session.ini"
display=$((50 + RANDOM % 50))
gtk4-broadwayd ":$display" >/dev/null 2>&1 &
broadway=$!
cleanup() {
  [[ -n "${tcode:-}" ]] && kill -TERM "$tcode" 2>/dev/null
  sleep 1
  kill "$broadway" 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT
sleep 0.5

# The private D-Bus session starts no services, and GTK uses no portals: a document portal started
# there mounted itself over the desktop's /run/user/<uid>/doc and unmounted it when the run ended,
# and GNOME Shell crashed right then.
cat > "$work/session.conf" <<'CONF'
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF

# Relative paths are taken from the repo, like t-code started there.
(cd "$root" && GDK_BACKEND=broadway BROADWAY_DISPLAY=":$display" XDG_CONFIG_HOME="$work/config" \
  GDK_DEBUG=no-portals GTK_USE_PORTAL=0 \
  exec dbus-run-session --config-file="$work/session.conf" -- "$root/build/t-code" "$@") > "$work/t-code.log" 2>&1 &

for _ in $(seq 50); do
  tcode=$(pgrep -n -x t-code -U "$(id -u)" || true)
  [[ -n "$tcode" ]] && grep -qa "XDG_CONFIG_HOME=$work" "/proc/$tcode/environ" 2>/dev/null && break
  tcode=
  sleep 0.1
done
[[ -n "$tcode" ]] || { echo "t-code did not start:" >&2; cat "$work/t-code.log" >&2; exit 1; }
echo "t-code $tcode on Broadway :$display, measuring in ${wait}s"
sleep "$wait"
"$root/tools/proc-report.sh" "$sample" "$tcode"
problems=$(grep -E 'CRITICAL|WARNING' "$work/t-code.log" || true)
if [[ -n "$problems" ]]; then
  echo
  echo "--- t-code log ---"
  head -20 <<< "$problems"
fi
