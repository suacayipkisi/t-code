#!/usr/bin/env python3
"""The transforms alone: python3 test_text_tools.py (t-code's test_window runs the plugin itself)."""
import unittest

from text_tools import TRANSFORMS, end_of


class Transforms(unittest.TestCase):
    def check(self, command, text, expected):
        self.assertEqual(TRANSFORMS[command](text), expected)

    def test_case(self):
        self.check("upper", "çağrı x", "ÇAĞRI X")
        self.check("title", "hello wORLD foo_bar", "Hello World Foo_Bar")
        self.check("snake", "someHTTPValue here", "some_httpvalue_here")
        self.check("camel", "make it_camel", "makeItCamel")

    def test_lines(self):
        self.check("sort", "b\na\nc\n", "a\nb\nc\n")
        self.check("sort-reverse", "b\na\nc", "c\nb\na")
        self.check("unique", "a\nb\na\nb\nc\n", "a\nb\nc\n")
        self.check("reverse", "1\n2\n3\n", "3\n2\n1\n")
        self.check("trim", "a  \nb\t\n", "a\nb\n")

    def test_codes(self):
        self.check("base64-encode", "hi ✓", "aGkg4pyT")
        self.check("base64-decode", "aGkg4pyT\n", "hi ✓")
        self.check("url-encode", "a b/ç", "a%20b%2F%C3%A7")
        self.check("url-decode", "a%20b", "a b")
        self.check("json-pretty", '{"a":[1,2]}', '{\n  "a": [\n    1,\n    2\n  ]\n}')
        self.check("json-minify", '{ "a" : 1 }', '{"a":1}')
        with self.assertRaises(ValueError):
            TRANSFORMS["json-pretty"]("{nope")

    def test_end_of(self):
        self.assertEqual(end_of("ab\ncd\n"), [3, 1])
        self.assertEqual(end_of("çağ"), [1, 4])


if __name__ == "__main__":
    unittest.main()
