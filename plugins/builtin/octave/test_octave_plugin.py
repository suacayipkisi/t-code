#!/usr/bin/env python3
"""Octave's reports turned into diagnostics (the formats measured on Octave 10.3): python3 -m
unittest in this folder."""
import os
import tempfile
import unittest

from mlang import Octave

octave = Octave()


class Report(unittest.TestCase):
    def setUp(self):
        self.project = tempfile.mkdtemp()
        self.script = os.path.join(self.project, "script2.m")
        self.helper = os.path.join(self.project, "lib", "helper.m")

    def test_runtime_error_and_frames(self):
        report = (f"error: 'b' undefined near line 2, column 11\n"
                  f"    helper at line 2 column 3 ({self.helper})\n"
                  f"    script2 at line 3 column 1 ({self.script})\n"
                  f"    run at line 93 column 5 (/usr/share/octave/10.3.0/m/miscellaneous/run.m)\n")
        found = octave.parse_report(report, self.script, self.project)
        self.assertEqual(sorted(found), sorted([self.helper, self.script]))
        self.assertEqual(found[self.helper][0]["range"], [2, 3])
        self.assertEqual(found[self.helper][0]["message"], "'b' undefined near line 2, column 11")
        self.assertEqual(found[self.helper][0]["severity"], "error")
        self.assertEqual(found[self.helper][0]["source"], "octave")
        self.assertEqual(found[self.script][0]["severity"], "info")
        self.assertIn("called from here", found[self.script][0]["message"])

    def test_syntax_error(self):
        report = (f"error: syntax error near line 2, column 5 in file {self.script}\n"
                  "    which at line 51 column 3 (/usr/share/octave/10.3.0/m/help/which.m)\n")
        found = octave.parse_report(report, self.script, self.project)
        self.assertEqual(list(found), [self.script])
        self.assertEqual(found[self.script][0]["range"], [2, 5])
        # older Octave: "parse error near line 2 of file ..."
        found = octave.parse_report(f"error: parse error near line 4 of file {self.script}\n", self.script,
                                    self.project)
        self.assertEqual(found[self.script][0]["range"], [4, 1])

    def test_no_error(self):
        self.assertEqual(octave.parse_report("ans = 2\n", self.script, self.project), {})

    def test_lines(self):
        line = octave.run_line("/p/a b/x.m", "/r/d.diary", "S", "D")
        self.assertIn("cd('/p/a b'); diary('/r/d.diary'); disp('S');", line)
        self.assertIn("run('x.m')", line)
        self.assertNotIn("getReport", line)
        self.assertNotIn("\n", line)  # one line: typed into the session
        self.assertEqual(octave.batch_command("/usr/bin/octave", "disp(1)"),
                         "/usr/bin/octave --no-gui --quiet --eval 'disp(1)'")


if __name__ == "__main__":
    unittest.main()
