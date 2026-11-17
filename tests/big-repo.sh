#!/bin/bash
# t-code on a big repository: build/t-code runs on its own Xvfb with a project of tens of thousands of
# files, a 3 MB source file, a long Markdown file and thousands of uncommitted changes, and each step
# (opening the project, Go to File, Search in Files, Source Control, the big file, its preview, typing,
# saving) is timed by how long the window stops answering: a heartbeat asks the control socket
# (get-project) every 20 ms, and the longest wait during a step is that step's stall.
#   tests/big-repo.sh                 a generated repository (BIG_FILES files, 20000 by default)
#   tests/big-repo.sh <repo>          an existing one instead, only read (no file in it is changed)
# A step fails when the window stopped for longer than BIG_LIMIT_MS (1000 by default). Xvfb draws in
# software: times are higher than on a desktop, compare runs on the same machine.
# Everything runs in a temporary HOME: the user's settings, session and log are never touched.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for tool in Xvfb xdotool git dbus-run-session python3 pgrep; do
  command -v "$tool" >/dev/null || { echo "1..0 # SKIP $tool is not installed"; exit 0; }
done
[[ -x "$root/build/t-code" ]] || make -C "$root" >/dev/null
limit=${BIG_LIMIT_MS:-1000}
files=${BIG_FILES:-20000}

work=$(mktemp -d)
export HOME="$work/home" XDG_CONFIG_HOME="$work/home/.config" XDG_STATE_HOME="$work/home/.local/state"
export XDG_CACHE_HOME="$work/home/.cache" XDG_DATA_HOME="$work/home/.local/share"
# A short folder for the socket: a Unix socket's path has at most 107 bytes.
export XDG_RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tcb.XXXXXX")
mkdir -p "$XDG_CONFIG_HOME/t-code" && chmod 700 "$XDG_RUNTIME_DIR"
printf '[settings]\ncheck-updates=false\nreopen-last-project=false\nautosave=false\n' > "$XDG_CONFIG_HOME/t-code/session.ini"

# The project: generated (folders of small C files, a big one, a long Markdown file, one commit and
# then thousands of changed files), or the one given.
generated=true
if [[ $# -gt 0 ]]; then
  project=$(cd "$1" && pwd) || exit 1
  generated=false
else
  project="$work/project"
  echo "# generating $files files"
  python3 -I - "$project" "$files" <<'PY'
import os, sys
root, count = sys.argv[1], int(sys.argv[2])
per_dir = 100
for i in range(count):
    folder = os.path.join(root, "src", f"module_{i // per_dir}")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, f"file_{i // per_dir}_{i % per_dir}.c"), "w") as f:
        f.write(f'#include <stdio.h>\n\nint common_symbol_{i}(int x)\n{{\n  return x * {i};\n}}\n')
with open(os.path.join(root, "big.c"), "w") as f:  # about 3 MB
    for i in range(60000):
        f.write(f"int big_function_{i}(int value) {{ return value + {i}; }} /* filler text */\n")
with open(os.path.join(root, "big.md"), "w") as f:  # sections with tables and code blocks
    for i in range(400):
        f.write(f"## Part {i}\n\nSome `code` and **bold** text.\n\n| Key | What |\n|---|---|\n")
        for row in range(8):
            f.write(f"| `Ctrl+{row}` | does thing {row} with a few more words |\n")
        f.write("\n```c\nint main(void) { return 0; }\n```\n\n")
PY
  git -C "$project" init -q -b main
  git -C "$project" add -A
  git -C "$project" -c user.name=t -c user.email=t@t commit -qm first
  python3 -I - "$project" "$files" <<'PY'
import os, sys
root, count = sys.argv[1], int(sys.argv[2])
for i in range(0, count, 10):  # a tenth of the files changed
    path = os.path.join(root, "src", f"module_{i // 100}", f"file_{i // 100}_{i % 100}.c")
    with open(path, "a") as f:
        f.write("/* changed */\n")
PY
fi

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
  [[ -n "${beat:-}" ]] && kill "$beat" 2>/dev/null
  [[ -n "${sampler:-}" ]] && kill "$sampler" 2>/dev/null
  [[ -n "${tcode:-}" ]] && kill -TERM "$tcode" 2>/dev/null
  sleep 0.5
  kill "$xvfb" 2>/dev/null
  rm -rf "$work" "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT
sleep 1

keys() { xdotool key --clearmodifiers --delay 60 "$@"; }
type_text() { xdotool type --clearmodifiers --delay 30 "$1"; }
send() {
  python3 -I -c '
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(sys.argv[2].encode() + b"\0")
s.settimeout(30)
out = b""
try:
    while chunk := s.recv(65536):
        out += chunk
        if out.endswith(b"\n") or out.endswith(b"\0"):
            break
except socket.timeout:
    pass
sys.stdout.write(out.decode().rstrip("\n\0"))
' "$XDG_RUNTIME_DIR/t-code-$tcode.sock" "$*"
}
now() { python3 -I -c 'import time; print(time.monotonic())'; }
steps="$work/steps" stalls="$work/stalls"
: > "$steps"
# <name>: the steps after it are timed apart. BIG_SAMPLE=<file>: the main thread's stacks too, a
# few times a second (gdb), each under its step's name.
step() {
  echo "$(now) $1" >> "$steps"
  [[ -n "${sampler:-}" ]] && kill "$sampler" 2>/dev/null
  if [[ -n "${BIG_SAMPLE:-}" && -n "${tcode:-}" ]]; then
    (echo "== $1"
      while sleep 0.3; do
        gdb -p "$tcode" -batch -ex 'thread 1' -ex 'bt 40' 2>/dev/null | grep '^#' |
          sed -E 's/^#[0-9]+ +(0x[0-9a-f]+ in )?//; s/ \(.*//' | paste -sd'<'
      done) >> "$BIG_SAMPLE" &
    sampler=$!
  fi
}

step "the project opens"
(cd "$project" && GDK_BACKEND=x11 GDK_DEBUG=no-portals GTK_USE_PORTAL=0 GTK_A11Y=none \
  exec dbus-run-session --config-file="$work/session.conf" -- "$root/build/t-code" "$project") \
  > "$work/stderr.log" 2>&1 &
for _ in $(seq 300); do
  tcode=$(pgrep -n -x t-code -U "$(id -u)" || true)
  [[ -n "$tcode" ]] && grep -qa "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" "/proc/$tcode/environ" 2>/dev/null &&
    [[ -S "$XDG_RUNTIME_DIR/t-code-$tcode.sock" ]] && break
  tcode=
  sleep 0.1
done
[[ -n "$tcode" ]] || { echo "Bail out! t-code did not start"; cat "$work/stderr.log"; exit 1; }

# The heartbeat: every round trip of the socket longer than 50 ms, with when it started.
python3 -I - "$XDG_RUNTIME_DIR/t-code-$tcode.sock" "$stalls" <<'PY' &
import socket, sys, time
path, out = sys.argv[1], open(sys.argv[2], "w", buffering=1)
while True:
    start = time.monotonic()
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(path)
        s.sendall(b"get-project\0")
        s.settimeout(60)
        s.recv(65536)
        s.close()
    except OSError:
        break
    took = time.monotonic() - start
    if took > 0.05:
        out.write(f"{start} {took}\n")
    time.sleep(0.02)
PY
beat=$!

window=$(xdotool search --sync --onlyvisible --pid "$tcode" | tail -1)
xdotool windowsize "$window" 1400 900 windowmove "$window" 0 0 windowfocus --sync "$window"
sleep 8 # the Files panel, git status, the file list for Go to File

if $generated; then
  name=file_150_42.c
  target="$project/src/module_150/$name"
  big="$project/big.c" markdown="$project/big.md"
else
  target=$(git -C "$project" ls-files | grep -E '\.(c|h|cpp|cc|py|rs|go)$' | tail -1)
  target="$project/$target" name=$(basename "$target")
  big=$(git -C "$project" ls-files -z | xargs -0 -I{} stat -c '%s {}' "$project/{}" 2>/dev/null |
    sort -n | tail -1 | cut -d' ' -f2-)
  markdown=$(git -C "$project" ls-files -z '*.md' | xargs -0 -I{} stat -c '%s {}' "$project/{}" 2>/dev/null |
    sort -n | tail -1 | cut -d' ' -f2-)
fi

step "Go to File finds a file among all of them"
keys ctrl+p
sleep 0.5
type_text "$name"
sleep 1
keys Return
sleep 2
opened=$(send get-file)

step "Search in Files over the whole project"
keys ctrl+shift+f
sleep 0.5
type_text "common_symbol"
keys Return
sleep 6

step "Source Control lists the changes"
keys ctrl+shift+g
sleep 6

step "the biggest file opens"
send "open $big" > /dev/null
sleep 5

step "the biggest file scrolls to its end"
keys ctrl+End
sleep 2
keys ctrl+Home
sleep 2

if [[ -n "$markdown" ]]; then
  step "the biggest Markdown file's preview opens"
  send "preview $markdown" > /dev/null
  sleep 5
  step "the preview scrolls through"
  for _ in $(seq 20); do keys Next; sleep 0.1; done
  sleep 2
fi

if $generated; then
  step "typing in the biggest file"
  send "open $big" > /dev/null
  sleep 1
  keys ctrl+End
  type_text "int typed_here(void) { return 1; }"
  sleep 2
  step "saving the biggest file"
  keys ctrl+s
  sleep 3
fi
step end
sleep 0.5

# Each step's longest stall, as TAP.
python3 -I - "$steps" "$stalls" "$limit" "$opened" "$target" <<'PY'
import sys
steps = [line.rstrip("\n").split(" ", 1) for line in open(sys.argv[1])]
stalls = [tuple(map(float, line.split())) for line in open(sys.argv[2]) if line.strip()]
limit, opened, target = float(sys.argv[3]), sys.argv[4], sys.argv[5]
n = failed = 0
for (start, name), (end, _) in zip(steps, steps[1:]):
    start, end = float(start), float(end)
    worst = max([took for at, took in stalls if start <= at < end], default=0.0) * 1000
    total = sum(took for at, took in stalls if start <= at < end) * 1000
    n += 1
    ok = worst <= limit
    failed += not ok
    print(f"{'ok' if ok else 'not ok'} {n} {name}: longest stall {worst:.0f} ms, {total:.0f} ms in all")
    if name.startswith("Go to File"):
        n += 1
        ok = opened == target
        failed += not ok
        print(f"{'ok' if ok else 'not ok'} {n}   ... and opens it" + ("" if ok else f" (got {opened!r})"))
print(f"1..{n}")
sys.exit(1 if failed else 0)
PY
