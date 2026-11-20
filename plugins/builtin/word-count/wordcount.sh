#!/usr/bin/env bash
set -euo pipefail

FILE="${TCODE_ACTIVE_FILE:-}"

if [[ -n "$FILE" && -f "$FILE" ]]; then
  WORDS=$(wc -w < "$FILE" | tr -d ' ')
  LINES=$(wc -l < "$FILE" | tr -d ' ')
  MSG="Word Count: $WORDS words, $LINES lines"
else
  MSG="Word Count: No active saved file"
fi

# "hello tapi_1 <token>": the token tells t-code which plugin speaks (without one, a script's hello)
HELLO="hello tapi_1${TCODE_PLUGIN_TOKEN:+ $TCODE_PLUGIN_TOKEN}"

if [[ -n "${TCODE_SOCKET:-}" && -S "${TCODE_SOCKET:-}" ]]; then
  if command -v python3 >/dev/null 2>&1; then
    # The Python stays inside single quotes: it must not use any itself
    python3 -c '
import os, socket, sys
hello, msg = sys.argv[1], sys.argv[2]
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.environ["TCODE_SOCKET"])
    s.sendall(f"{hello}\nsay {msg}\n".encode("utf-8"))
    answers = s.makefile("r", encoding="utf-8")
    greeting = answers.readline().strip()  # "ok tapi_1 t-code <version>"
    # after a refused hello the connection is a legacy one, and say gets no answer
    answer = answers.readline().strip() if greeting.startswith("ok") else greeting
    if answer != "ok":
        print(f"word-count: {answer}", file=sys.stderr)
    s.close()
except OSError as e:
    print(f"word-count: {e}", file=sys.stderr)
' "$HELLO" "$MSG"
  elif command -v nc >/dev/null 2>&1; then
    printf '%s\nsay %s\n' "$HELLO" "$MSG" | nc -U "$TCODE_SOCKET" >/dev/null 2>&1 || true
  fi
fi

echo "$MSG"
