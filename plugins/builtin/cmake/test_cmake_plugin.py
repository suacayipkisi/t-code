#!/usr/bin/env python3
"""CMake's output turned into diagnostics: python3 test_cmake_plugin.py (t-code's test_window runs the
plugin against a fake cmake)."""
import unittest

from cmake_plugin import configure_argv, parse_output, parse_presets

OUTPUT = """-- The C compiler identification is GNU 16.2.1
CMake Error at sub/CMakeLists.txt:2 (foo_bar):
  Unknown CMake command "foo_bar".


CMake Warning (dev) at CMakeLists.txt:1 (project):
  cmake_minimum_required() should be called first.
This warning is for project developers.  Use -Wno-dev to suppress it.

CMake Error at /usr/share/cmake/Modules/FindFoo.cmake:9 (message):
  Could not find Foo:

    libfoo.so

Call Stack (most recent call first):
  CMakeLists.txt:7 (find_package)


CMake Error at CMakeLists.txt:4 (add_executable):

-- Configuring incomplete, errors occurred!
CMake Error: The source directory "/nope" does not exist.
"""


class Output(unittest.TestCase):
    def test_messages(self):
        found, general = parse_output(OUTPUT, "/p")
        self.assertEqual(sorted(found), ["/p/CMakeLists.txt", "/p/sub/CMakeLists.txt", "/usr/share/cmake/Modules/FindFoo.cmake"])
        self.assertEqual(found["/p/sub/CMakeLists.txt"],
                         [{"range": [2, 1], "severity": "error", "message": 'Unknown CMake command "foo_bar".',
                           "source": "cmake"}])
        warning, empty = found["/p/CMakeLists.txt"]
        self.assertEqual(warning["severity"], "warning")
        self.assertEqual(warning["message"], "cmake_minimum_required() should be called first.")
        self.assertEqual(empty["message"], "add_executable() failed")
        self.assertEqual(found["/usr/share/cmake/Modules/FindFoo.cmake"][0]["message"],
                         "Could not find Foo: libfoo.so")
        self.assertEqual(general, ['The source directory "/nope" does not exist.'])

    def test_presets(self):
        text = 'Available configure presets:\n\n  "dev" - Dev build\n  "rel"\n\n'
        self.assertEqual(parse_presets(text), [("dev", "Dev build"), ("rel", None)])

    def test_argv(self):
        self.assertEqual(configure_argv("/p", {"preset": "dev"}), ["cmake", "-S", "/p", "--preset", "dev"])
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", configure_argv("/p", {}))
        self.assertEqual(configure_argv("/p", {})[4], "/p/build")


if __name__ == "__main__":
    unittest.main()
