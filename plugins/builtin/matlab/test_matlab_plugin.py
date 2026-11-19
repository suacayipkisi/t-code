#!/usr/bin/env python3
"""MATLAB's reports turned into diagnostics: python3 test_matlab_plugin.py (t-code's test_window runs
the plugin against a fake MATLAB)."""
import os
import tempfile
import unittest

from matlab_plugin import parse_report, quoted, run_line


class Report(unittest.TestCase):
    def setUp(self):
        self.project = tempfile.mkdtemp()
        self.script = os.path.join(self.project, "script1.m")
        os.makedirs(os.path.join(self.project, "lib"))
        self.helper = os.path.join(self.project, "lib", "helper.m")
        for path in (self.script, self.helper):
            open(path, "w").close()

    def test_runtime_error(self):
        report = ("Undefined function or variable 'x'.\n\nError in script1 (line 2)\ny = x + 1;\n\n"
                  "Error in run (line 91)\nevalin('caller', strcat(script, ';'));\n")
        found = parse_report(report, self.script, self.project)
        self.assertEqual(list(found), [self.script])
        self.assertEqual(found[self.script][0]["range"], [2, 1])
        self.assertEqual(found[self.script][0]["message"], "Undefined function or variable 'x'.")
        self.assertEqual(found[self.script][0]["severity"], "error")

    def test_error_using_and_frames(self):
        report = ("Error using helper\nNot enough input arguments.\n\nError in helper (line 4)\nz = a + b;\n\n"
                  "Error in script1 (line 7)\nhelper(1)\n")
        found = parse_report(report, self.script, self.project)
        self.assertEqual(found[self.helper][0]["message"], "Not enough input arguments.")
        self.assertEqual(found[self.helper][0]["range"], [4, 1])
        self.assertEqual(found[self.script][0]["severity"], "info")
        self.assertIn("called from here", found[self.script][0]["message"])

    def test_syntax_error(self):
        report = f"Error: File: {self.script} Line: 3 Column: 5\nInvalid expression.\n"
        found = parse_report(report, self.script, self.project)
        self.assertEqual(found[self.script][0]["range"], [3, 5])
        self.assertEqual(found[self.script][0]["message"], "Invalid expression.")

    def test_line(self):
        self.assertEqual(quoted("it's"), "'it''s'")
        line = run_line("/p/a b/x.m", "/r/d.diary", "S", "D")
        self.assertIn("cd('/p/a b'); diary('/r/d.diary'); disp('S');", line)
        self.assertIn("run('x.m')", line)
        self.assertIn("disp('D'); diary off", line)


if __name__ == "__main__":
    unittest.main()
