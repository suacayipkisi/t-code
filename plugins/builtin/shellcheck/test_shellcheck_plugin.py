#!/usr/bin/env python3
"""The plugin's own logic: python3 test_shellcheck_plugin.py (t-code's test_window runs it for real)."""
import os
import tempfile
import unittest

from shellcheck_plugin import character_column, diagnostics, is_script


class Logic(unittest.TestCase):
    def test_is_script(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name, text, expected in [("a.sh", "", True), ("tool", "#!/usr/bin/env bash\n", True),
                                         ("t", "#!/bin/dash -e\n", True), ("p", "#!/usr/bin/python3\n", False),
                                         ("notes.txt", "echo hi\n", False)]:
                path = os.path.join(tmp, name)
                with open(path, "w") as f:
                    f.write(text)
                self.assertEqual(is_script(path), expected, name)

    def test_tabs(self):
        self.assertEqual(character_column("\techo $x", 9), 2)
        self.assertEqual(character_column("a\tb", 9), 3)
        self.assertEqual(character_column("abc", 3), 3)

    def test_diagnostics(self):
        with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
            f.write("#!/bin/sh\n\techo $1\n")
        findings = {"comments": [{"line": 2, "column": 14, "endLine": 2, "endColumn": 16, "level": "info",
                                  "code": 2086, "message": "Double quote to prevent globbing and word splitting."},
                                 {"line": 1, "column": 1, "level": "style", "code": 2148, "message": "x"}]}
        items = diagnostics(f.name, findings)
        os.unlink(f.name)
        self.assertEqual(items[0]["range"], [2, 7, 2, 9])
        self.assertEqual(items[0]["severity"], "info")
        self.assertEqual(items[0]["code"], "SC2086")
        self.assertEqual(items[1]["severity"], "hint")
        self.assertEqual(items[1]["range"], [1, 1, 1, 1])


if __name__ == "__main__":
    unittest.main()
