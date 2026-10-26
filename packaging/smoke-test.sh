# Runs as a normal user inside a fresh container with the t-code package installed (test.sh):
# checks the installed files, starts t-code on a headless X server with five C files and checks
# that clangd (with fewer malloc arenas) comes up, that a Markdown preview
# opens, and that t-code quits cleanly.
set -uo pipefail

failures=0
pass() { printf '  \033[1;32mok\033[0m   %s\n' "$1"; }
fail() { printf '  \033[1;31mFAIL\033[0m %s\n' "$1"; failures=$((failures + 1)); }
check() { # <description> <command...>
  local what=$1
  shift
  if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi
}

TCODE_BIN="${TCODE_BIN:-t-code}"
share=/usr/share/t-code

echo "Installed files"
check "t-code --version prints 't-code $VERSION'" test "$("$TCODE_BIN" --version)" = "t-code $VERSION"
if [[ "$TCODE_BIN" == "t-code" ]]; then
  check "the editor's style scheme in $share/styles" test -f "$share/styles/dark-classic.xml"
  check "the built-in snippets in $share/snippets" test -f "$share/snippets/c.snippets"
  check "desktop entry is valid" desktop-file-validate /usr/share/applications/io.github.tcode.desktop
  check "icon installed" test -f /usr/share/icons/hicolor/scalable/apps/io.github.tcode.svg
fi
for cmd in clangd git; do
  check "dependency '$cmd' installed" command -v "$cmd"
done
if [[ -n "${NO_FONT_PACKAGE:-}" ]]; then
  check "a monospace font for the Cascadia Code fallback" sh -c "fc-match -f '%{spacing}' monospace | grep -q 100"
else
  check "Cascadia Code font installed" sh -c "fc-match -f '%{family}' 'Cascadia Code' | grep -q 'Cascadia Code'"
fi

echo "Running t-code"
export XDG_RUNTIME_DIR="/tmp/runtime-$USER" DISPLAY=:99 GDK_BACKEND=x11 GSK_RENDERER=cairo GTK_A11Y=none
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
project="$HOME/project"
mkdir -p "$project"
printf '#include <stdio.h>\n\nint main(void)\n{\n    printf("hi\\n");\n    return 0;\n}\n' > "$project/main.c"
for f in a b c d; do printf 'int %s(void) { return 0; }\n' "$f" > "$project/$f.c"; done
git -C "$project" init -q
Xvfb :99 -screen 0 1600x1000x24 >/dev/null 2>&1 &
xvfb=$!
sleep 2
(cd "$project" && exec dbus-run-session -- "$TCODE_BIN" "$project"/{a,b,c,d}.c "$project/main.c") > /tmp/t-code.log 2>&1 &
app=$!
# clangd starts when the shown C file has loaded; give it time. (clangd names its main thread
# "clangd.main", so it is found by its command line: "clangd ..." or, found on PATH, "/usr/bin/clangd ...".)
for _ in $(seq 30); do pgrep -u "$USER" -f '^(/[^ ]*/)?clangd ' >/dev/null && break; sleep 1; done

tcode=$(pgrep -u "$USER" -x "${TCODE_PROCESS_NAME:-t-code}" | head -1)
check "t-code is running" test -n "$tcode"
check "clangd started" pgrep -u "$USER" -f '^(/[^ ]*/)?clangd '
check "one clangd for all five editors" test "$(pgrep -u "$USER" -fc '^(/[^ ]*/)?clangd ')" -eq 1
check "clangd runs with 2 malloc arenas" \
  sh -c "tr '\\0' '\\n' < /proc/\$(pgrep -u $USER -f '^(/[^ ]*/)?clangd ' | head -1)/environ | grep -q arena_max=2"
check "control socket created" sh -c "ls $XDG_RUNTIME_DIR/t-code-*.sock"
# The Markdown preview: rendered natively in-process (no WebKit process).
printf '# Notes\n\nhello\n' > "$project/README.md"
python3 -I -c 'import socket, sys; s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.send(sys.argv[2].encode())' \
  "$XDG_RUNTIME_DIR/t-code-$tcode.sock" "preview $project/README.md"$'\n' 2>/dev/null
sleep 1
check "Markdown preview opens in-process" kill -0 "$tcode"
kill -TERM "${tcode:-0}" 2>/dev/null
for _ in $(seq 10); do kill -0 "$app" 2>/dev/null || break; sleep 1; done
check "t-code quits on SIGTERM" sh -c "! kill -0 ${tcode:-0} 2>/dev/null"
check "control socket removed on quit" sh -c "! ls $XDG_RUNTIME_DIR/t-code-*.sock"
check "no GTK criticals in the log" sh -c "! grep -E 'CRITICAL|segfault|Segmentation' /tmp/t-code.log"
kill "$xvfb" 2>/dev/null

if ((failures)); then
  echo "--- t-code log ---"
  cat /tmp/t-code.log
  echo "$failures check(s) failed"
  exit 1
fi
echo "All checks passed"
