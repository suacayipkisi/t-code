#!/bin/bash
# Real-use scenarios: build/t-code runs on a virtual X screen (Xvfb) and gets real key presses and
# mouse clicks from xdotool, as a user would give them. What happened is checked on disk (the files,
# git) and through the control socket (t-code --send get-file / get-text); at the end the window is
# closed with its title bar's close button, and t-code's log file must hold no warning.
#   tests/scenarios.sh            (needs Xvfb, xdotool, git, dbus-run-session, python3)
# Everything runs in a temporary HOME: the user's settings, session and log are never touched.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for tool in Xvfb xdotool git dbus-run-session python3 pgrep; do
  command -v "$tool" >/dev/null || { echo "1..0 # SKIP $tool is not installed"; exit 0; }
done
[[ -x "$root/build/t-code" ]] || make -C "$root" >/dev/null

work=$(mktemp -d)
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config" XDG_STATE_HOME="$work/home/.local/state"
export XDG_CACHE_HOME="$work/home/.cache" XDG_DATA_HOME="$work/home/.local/share" XDG_RUNTIME_DIR="$work/run"
mkdir -p "$XDG_CONFIG_HOME/t-code" "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
printf '[settings]\ncheck-updates=false\nreopen-last-project=false\nautosave=false\n' > "$XDG_CONFIG_HOME/t-code/session.ini"
log="$XDG_STATE_HOME/t-code/t-code.log"

# The project: a C file, a header and a Markdown file in a git repository with one commit.
project="$work/project"
mkdir -p "$project"
printf '#include "util.h"\n\nint main(void)\n{\n  return twice(1);\n}\n' > "$project/main.c"
printf 'static inline int twice(int x) { return 2 * x; }\n' > "$project/util.h"
printf '# Notes\n\nSome *text*.\n' > "$project/notes.md"
git -C "$project" init -q -b main
git -C "$project" -c user.name=t -c user.email=t@t add -A
git -C "$project" -c user.name=t -c user.email=t@t commit -qm first

display=$((100 + RANDOM % 100))
Xvfb ":$display" -screen 0 1600x1000x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
export DISPLAY=":$display"
# As tools/headless.sh: a private D-Bus session that starts no services, and no portals.
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
cleanup() {
  [[ -n "${tcode:-}" ]] && kill -TERM "$tcode" 2>/dev/null
  sleep 0.5
  kill "$xvfb" 2>/dev/null
  rm -rf "$work"
}
trap cleanup EXIT
sleep 1

n=0 failed=0
ok() { n=$((n + 1)); echo "ok $n $1"; }
not_ok() { n=$((n + 1)); failed=$((failed + 1)); echo "not ok $n $1"; [[ -n "${2:-}" ]] && sed 's/^/# /' <<< "$2"; }
check() { # <name> <command...>
  local name=$1
  shift
  if "$@"; then ok "$name"; else not_ok "$name"; fi
}
# Runs a command until it succeeds, for up to 5 seconds.
eventually() {
  for _ in $(seq 50); do "$@" && return 0; sleep 0.1; done
  return 1
}
# One request over the control socket, ended with a NUL (the text may hold line breaks); prints the
# answer (get-file, get-project, get-text) without its line break. t-code --send does not read one.
send() {
  python3 -c '
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(sys.argv[2].encode() + b"\0")
s.settimeout(0.3)
out = b""
try:
    while chunk := s.recv(65536):
        out += chunk
except socket.timeout:
    pass
sys.stdout.write(out.decode().rstrip("\n\0"))
' "$XDG_RUNTIME_DIR/t-code-$tcode.sock" "$*"
}
keys() { xdotool key --clearmodifiers --delay 60 "$@"; sleep 0.3; }
type_text() { xdotool type --clearmodifiers --delay 30 "$1"; sleep 0.3; }
file_has() { grep -qF -- "$2" "$1"; }
current_is() { [[ "$(send get-file)" == "$1" ]]; }

(cd "$project" && GDK_BACKEND=x11 GDK_DEBUG=no-portals GTK_USE_PORTAL=0 GTK_A11Y=none \
  exec dbus-run-session --config-file="$work/session.conf" -- "$root/build/t-code" "$project") \
  > "$work/stderr.log" 2>&1 &
for _ in $(seq 100); do
  tcode=$(pgrep -n -x t-code -U "$(id -u)" || true)
  [[ -n "$tcode" ]] && grep -qa "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" "/proc/$tcode/environ" 2>/dev/null && break
  tcode=
  sleep 0.1
done
[[ -n "$tcode" ]] || { echo "Bail out! t-code did not start"; cat "$work/stderr.log"; exit 1; }
window=$(xdotool search --sync --onlyvisible --pid "$tcode" | tail -1)
xdotool windowsize "$window" 1400 900 windowmove "$window" 0 0 windowfocus --sync "$window"
sleep 1
check "the window opens and its socket answers" eventually test -n "$(send get-project)"

# Go to File: Ctrl+P, the name, Enter.
keys ctrl+p
type_text "main.c"
keys Return
check "Ctrl+P opens main.c" eventually current_is "$project/main.c"

# A click into the text, typing at the end, Ctrl+S.
eval "$(xdotool getwindowgeometry --shell "$window")"
xdotool mousemove --window "$window" $((WIDTH / 2)) $((HEIGHT / 3)) click 1
sleep 0.3
keys ctrl+End
type_text "/* typed */"
keys ctrl+s
check "typing after a click is saved by Ctrl+S" eventually file_has "$project/main.c" "/* typed */"

# Multiple cursors: Ctrl+D twice on "twice" takes both, typing replaces both; Ctrl+Z undoes it.
send "replace-all int twice(int x);
int y = twice(2);
"
keys ctrl+Home Right Right Right Right Right
keys ctrl+d ctrl+d
type_text "half"
keys ctrl+s
check "Ctrl+D Ctrl+D and typing change both names" \
  eventually file_has "$project/main.c" "int y = half(2);"
check "  ... and the first one too" file_has "$project/main.c" "int half(int x);"
keys Escape ctrl+z ctrl+s
check "Ctrl+Z takes the change back" eventually file_has "$project/main.c" "int y = twice(2);"

# The command palette: Split Editor opens an empty right side, Ctrl+P puts a file there, Ctrl+W
# closes it and the side goes, back to main.c.
keys ctrl+shift+p
type_text "Split Editor"
keys Return
sleep 0.5
keys ctrl+p
type_text "util.h"
keys Return
check "a file opened after Split Editor goes to the new side" eventually current_is "$project/util.h"
keys ctrl+w
check "  ... and Ctrl+W closes it, back to main.c" eventually current_is "$project/main.c"

# Tabs: two more files, Ctrl+PageDown through them, Ctrl+W closes one.
send "open $project/util.h"
send "open $project/notes.md"
check "files opened over the socket" eventually current_is "$project/notes.md"
keys ctrl+Next
sleep 0.3
before=$(send get-file)
check "Ctrl+PageDown goes to another tab" test "$before" != "$project/notes.md"
keys ctrl+w
sleep 0.5
check "Ctrl+W closes the current tab" test "$(send get-file)" != "$before"

# Settings open with Ctrl+, and close with Escape.
keys ctrl+comma
sleep 1
settings=$(xdotool search --onlyvisible --pid "$tcode" | wc -l)
keys Escape
sleep 0.5
check "Ctrl+, opens Settings and Escape closes it" \
  test "$settings" -gt "$(xdotool search --onlyvisible --pid "$tcode" | wc -l)"
xdotool windowfocus --sync "$window"

# Git: stash a change through the palette, then pop it back.
send "open $project/main.c"
eventually current_is "$project/main.c"
send "append // stash me"
send save
eventually file_has "$project/main.c" "// stash me"
keys ctrl+shift+p
type_text "Git: Stash"
keys Return
sleep 0.5
type_text "work in progress"
keys Return
check "Git: Stash puts the change aside" eventually test -n "$(git -C "$project" stash list)"
check "  ... and the file is back as committed" eventually bash -c "! grep -q 'stash me' '$project/main.c'"
keys ctrl+shift+p
type_text "Git: Pop Stash"
keys Return
sleep 1
keys Return
check "Git: Pop Stash brings it back" eventually file_has "$project/main.c" "// stash me"
check "  ... and the stash is gone" eventually test -z "$(git -C "$project" stash list)"

# The title bar's close button (top right) closes the window and t-code quits.
eval "$(xdotool getwindowgeometry --shell "$window")"
xdotool mousemove --window "$window" $((WIDTH - 22)) 20 click 1
gone() { ! kill -0 "$tcode" 2>/dev/null; }
if eventually gone; then
  ok "the title bar's close button quits t-code"
else
  not_ok "the title bar's close button quits t-code"
  kill -TERM "$tcode"
  eventually gone
fi
tcode=

# Nothing went wrong on the way: the log holds this run's start and quit lines and nothing else.
problems=$(grep -v '^===' "$log" 2>/dev/null)
if [[ -z "$problems" ]]; then
  ok "t-code logged no warning"
else
  not_ok "t-code logged no warning" "$(head -20 <<< "$problems")"
fi
check "the log says t-code quit cleanly" grep -q '^=== quit' "$log"
echo "1..$n"
((failed == 0))
