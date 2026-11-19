#!/usr/bin/env python3
"""Text Tools for t-code: $1 is the command id. The selection (or, for line commands without one,
the whole buffer) is replaced with one `edit`, so a single undo brings it back."""
import base64
import binascii
import datetime
import json
import re
import sys
import urllib.parse
import uuid

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
from tcode import Tcode, TcodeError  # noqa: E402


def words(text):
    return re.findall(r"[A-Za-z0-9]+", re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", text))


def lines_of(text, change):
    """change() on the lines, keeping a final line break if there was one."""
    end = "\n" if text.endswith("\n") else ""
    return "\n".join(change(text[: len(text) - len(end)].split("\n"))) + end


def unique(lines):
    seen = set()
    return [line for line in lines if not (line in seen or seen.add(line))]


def b64decode(text):
    return base64.b64decode(text.strip(), validate=True).decode()


TRANSFORMS = {
    "upper": str.upper,
    "lower": str.lower,
    "title": lambda t: re.sub(r"[^\W_]+", lambda m: m.group(0).capitalize(), t),
    "snake": lambda t: "_".join(w.lower() for w in words(t)),
    "camel": lambda t: "".join(w.lower() if i == 0 else w.capitalize() for i, w in enumerate(words(t))),
    "sort": lambda t: lines_of(t, sorted),
    "sort-reverse": lambda t: lines_of(t, lambda ls: sorted(ls, reverse=True)),
    "unique": lambda t: lines_of(t, unique),
    "reverse": lambda t: lines_of(t, lambda ls: ls[::-1]),
    "trim": lambda t: lines_of(t, lambda ls: [line.rstrip() for line in ls]),
    "base64-encode": lambda t: base64.b64encode(t.encode()).decode(),
    "base64-decode": b64decode,
    "url-encode": lambda t: urllib.parse.quote(t, safe=""),
    "url-decode": urllib.parse.unquote,
    "json-pretty": lambda t: json.dumps(json.loads(t), indent=2, ensure_ascii=False),
    "json-minify": lambda t: json.dumps(json.loads(t), separators=(",", ":"), ensure_ascii=False),
}
WHOLE_BUFFER = {"sort", "sort-reverse", "unique", "reverse", "trim", "json-pretty", "json-minify"}
INSERTS = {
    "uuid": lambda: str(uuid.uuid4()),
    "date": lambda: datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
}


def end_of(text):
    """The place after text, 1-based: [line, column]."""
    lines = text.split("\n")
    return [len(lines), len(lines[-1]) + 1]


def main(command):
    t = Tcode()
    try:
        if command in INSERTS:
            t.request("insert", INSERTS[command]())
            return
        if command not in TRANSFORMS:
            t.request("say", f"Text Tools does not know '{command}'")
            return
        start_line, start_col, end_line, end_col = map(int, t.request("get-selection-range").split())
        if (start_line, start_col) == (end_line, end_col):
            if command not in WHOLE_BUFFER:
                t.request("say", "Select some text first: Text Tools needs something to chew on")
                return
            text = t.text("get-text")
            start_line, start_col = 1, 1
            end_line, end_col = end_of(text)
        else:
            text = t.text("get-selection")
        try:
            changed = TRANSFORMS[command](text)
        except (ValueError, binascii.Error, UnicodeDecodeError) as e:
            t.request("say", f"That does not look right for this one: {str(e)[:80]}")
            return
        if changed == text:
            t.request("say", "Already like that: nothing to do")
            return
        t.json("edit", [{"range": [start_line, start_col, end_line, end_col], "text": changed}])
    except TcodeError as e:
        if e.code == "no-editor":
            t.request("say", "Text Tools needs an editor: open a file first")
        else:
            raise
    finally:
        t.close()


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "")
