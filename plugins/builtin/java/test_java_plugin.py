#!/usr/bin/env python3
"""The Java plugin's pieces: python3 test_java_plugin.py (t-code's test_window runs the plugin against
a window, with a fake JDK). With a real JDK on PATH, a plain folder is built and run for real."""
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import java_plugin as jp
import jdtls_launcher as launcher

JAVAC_OUTPUT = """src/com/example/App.java:7: error: cannot find symbol
        int y = x + 1;
                ^
  symbol:   variable x
  location: class App
src/com/example/Util.java:3: warning: [unchecked] unchecked conversion
\tList<String> l = new ArrayList();
\t                 ^
2 errors
"""

MAVEN_OUTPUT = """[ERROR] COMPILATION ERROR :
[ERROR] /p/src/main/java/com/example/App.java:[12,9] cannot find symbol
[WARNING] /p/src/main/java/com/example/App.java:[3,1] deprecated
[ERROR] Failures:
[ERROR]   AppTest.adds:14 expected: <3> but was: <4>
[ERROR] Tests run: 2, Failures: 1, Errors: 0, Skipped: 0
"""

GRADLE_OUTPUT = """
AppTest > adds() FAILED
    org.opentest4j.AssertionFailedError at AppTest.java:14
"""

TRACE = """Exception in thread "main" java.lang.IllegalStateException: wrapped
\tat com.example.App.main(App.java:9)
Caused by: java.lang.ArithmeticException: / by zero
\tat com.example.Calc.div(Calc.java:5)
\tat java.base/java.util.Objects.requireNonNull(Objects.java:233)
\tat app//com.example.App.main(App.java:8)
\t... 1 more
some output after
"""


class Fixture(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def put(self, relative, text):
        path = os.path.join(self.dir, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
        return path


class Sources(Fixture):
    def test_class_and_root(self):
        app = self.put("src/com/example/App.java", "package com.example;\nclass App { public static void main(String[] a) {} }\n")
        loose = self.put("scratch/Hello.java", "package other.place;\nclass Hello {}\n")
        compact = self.put("Compact.java", 'void main() { IO.println("hi"); }\n')
        self.assertEqual(jp.class_of(app, jp.read(app)), "com.example.App")
        self.assertEqual(jp.source_root(app, jp.read(app)), os.path.join(self.dir, "src"))
        self.assertEqual(jp.source_root(loose, jp.read(loose)), os.path.join(self.dir, "scratch"))
        self.assertTrue(jp.has_main(jp.read(app)) and jp.has_main(jp.read(compact)))
        self.assertFalse(jp.has_main(jp.read(loose)))

    def test_detect(self):
        self.put("pom.xml", "<project/>")
        app = self.put("src/main/java/a/App.java", "package a;\n")
        test = self.put("src/test/java/a/AppTest.java", "package a;\n")
        build = jp.detect(app, self.dir)
        self.assertEqual((build.kind, build.root, build.tool, build.test), ("maven", self.dir, "mvn", False))
        self.assertTrue(jp.detect(test, self.dir).test)
        self.put("tool/settings.gradle", "")
        self.put("tool/lib/build.gradle", "")
        wrapper = self.put("tool/gradlew", "#!/bin/sh\n")
        os.chmod(wrapper, 0o755)
        lib = self.put("tool/lib/src/main/java/b/Lib.java", "package b;\n")
        build = jp.detect(lib, self.dir)
        self.assertEqual((build.kind, build.root, build.tool), ("gradle", os.path.join(self.dir, "tool/lib"), wrapper))
        plain = self.put("other/src/c/Main.java", "package c;\n")
        os.remove(os.path.join(self.dir, "pom.xml"))
        build = jp.detect(plain, os.path.join(self.dir, "other"))
        self.assertEqual((build.kind, build.source), ("plain", os.path.join(self.dir, "other/src")))

    def test_find_by_package(self):
        calc = self.put("src/main/java/com/example/Calc.java", "package com.example;\n")
        elsewhere = self.put("module/x/com/example/App.java", "package com.example;\n")
        sources = jp.Sources(self.dir, [os.path.join(self.dir, "src/main/java")])
        self.assertEqual(sources.find("com.example.Calc$Inner", "Calc.java"), calc)
        self.assertEqual(sources.find("com.example.App", "App.java"), elsewhere)
        self.assertIsNone(sources.find("java.util.Objects", "Objects.java"))


class Output(Fixture):
    def test_javac(self):
        found = jp.parse_build(JAVAC_OUTPUT, "/p", jp.Sources(self.dir, []))
        app, util = found["/p/src/com/example/App.java"], found["/p/src/com/example/Util.java"]
        self.assertEqual(app, [{"range": [7, 17], "severity": "error", "source": "javac",
                                "message": "cannot find symbol (symbol: variable x; location: class App)"}])
        self.assertEqual(util[0]["severity"], "warning")
        self.assertEqual(util[0]["range"], [3, 19])  # the caret line keeps the tab

    def test_maven_and_tests(self):
        test = self.put("src/test/java/AppTest.java", "class AppTest {}\n")
        found = jp.parse_build(MAVEN_OUTPUT, "/p", jp.Sources(self.dir, [os.path.join(self.dir, "src/test/java")]))
        app = found["/p/src/main/java/com/example/App.java"]
        self.assertEqual([d["range"] for d in app], [[12, 9], [3, 1]])
        self.assertEqual([d["severity"] for d in app], ["error", "warning"])
        self.assertEqual(found[test], [{"range": [14, 1], "severity": "error", "source": "test",
                                        "message": "adds: expected: <3> but was: <4>"}])
        found = jp.parse_build(GRADLE_OUTPUT, "/p", jp.Sources(self.dir, [os.path.join(self.dir, "src/test/java")]))
        self.assertEqual(found[test][0]["message"], "adds() failed: AssertionFailedError")

    def test_exceptions(self):
        app = self.put("src/com/example/App.java", "package com.example;\n")
        calc = self.put("src/com/example/Calc.java", "package com.example;\n")
        found = jp.parse_exceptions(TRACE, jp.Sources(self.dir, [os.path.join(self.dir, "src")]))
        self.assertEqual(found[app], [
            {"range": [9, 1], "severity": "error", "message": "IllegalStateException: wrapped", "source": "java"},
            {"range": [8, 1], "severity": "info", "message": "called from here: ArithmeticException: / by zero",
             "source": "java"}])
        self.assertEqual(found[calc], [{"range": [5, 1], "severity": "error",
                                        "message": "ArithmeticException: / by zero", "source": "java"}])
        self.assertEqual(jp.parse_exceptions("java.lang.Thing is not an exception\nhello\n",
                                             jp.Sources(self.dir, [])), {})


class Lines(Fixture):
    def test_maven_and_gradle(self):
        maven = jp.Build("maven", "/p", "mvn")
        first, then = jp.run_lines(maven, "/p", self.put("A.java", "package x;\nclass A {}\n"), self.dir)
        self.assertIn("compile dependency:build-classpath -Dmdep.outputFile=", first)
        self.assertTrue(then.startswith('java -cp "target/classes:$(cat ') and then.endswith(" x.A"))
        self.assertEqual(jp.test_line(maven, ""), "mvn -B test")
        test = jp.Build("maven", "/p", "mvn", test=True)
        self.assertIn("-Dtest=ATest", jp.test_line(test, self.put("ATest.java", "package x;\n")))
        gradle = jp.Build("gradle", "/p", "/p/gradlew")
        first, then = jp.run_lines(gradle, "/p", self.put("B.java", "class B {}\n"), self.dir)
        self.assertIn("tcodeClasspath", first)
        self.assertTrue(os.path.exists(os.path.join(self.dir, "classpath.gradle")))
        self.assertEqual(then, 'java -cp "$(cat build/tcode-classpath-main.txt)" B')

    def test_plain_build_and_run(self):
        """The script for real, when a JDK is installed: compile errors, then a run with an exception."""
        if not shutil.which("javac") or not shutil.which("java"):
            self.skipTest("no JDK")
        cache = mock.patch.dict(os.environ, {"XDG_CACHE_HOME": os.path.join(self.dir, "cache")})
        cache.start()
        self.addCleanup(cache.stop)
        main = self.put("src/com/example/App.java", "package com.example;\npublic class App {\n"
                        "  public static void main(String[] a) {\n    System.out.println(\"hi\");\n"
                        "    Calc.div(1, 0);\n  }\n}\n")
        self.put("src/com/example/Calc.java", "package com.example;\nclass Calc {\n"
                 "  static int div(int a, int b) { return a / b; }\n}\n")
        project = self.dir
        build = jp.detect(main, project)
        run_dir = os.path.join(self.dir, "run")
        os.makedirs(run_dir)
        first, then = jp.run_lines(build, project, main, run_dir)
        script = os.path.join(run_dir, "run.sh")
        with open(script, "w") as f:
            f.write(jp.script_text(build.root, run_dir, first, then))
        result = subprocess.run(["sh", script], capture_output=True, text=True)
        self.assertIn("hi", result.stdout)
        self.assertTrue(os.path.exists(os.path.join(run_dir, "done")))
        self.assertFalse(os.path.exists(script))  # it removed itself
        self.assertEqual(jp.read(os.path.join(run_dir, "status")).strip(), "1")
        found = jp.parse_exceptions(jp.read(os.path.join(run_dir, "run.log")), jp.Sources(project, [build.source]))
        calc = os.path.join(self.dir, "src/com/example/Calc.java")
        self.assertEqual(found[calc][0]["range"], [3, 1])
        self.assertEqual(found[main][0]["severity"], "info")

        self.put("src/com/example/Calc.java", "package com.example;\nclass Calc {\n"
                 "  static int div(int a, int b) { return a / c; }\n}\n")
        os.makedirs(run_dir, exist_ok=True)
        with open(script, "w") as f:
            f.write(jp.script_text(build.root, run_dir, first, then))
        subprocess.run(["sh", script], capture_output=True, text=True)
        found = jp.parse_build(jp.read(os.path.join(run_dir, "build.log")), build.root, jp.Sources(project, []))
        self.assertEqual(found[calc][0]["range"], [3, 45])
        self.assertEqual(jp.read(os.path.join(run_dir, "status")).strip(), "1")


class Launcher(Fixture):
    def test_finds_jdtls_and_java(self):
        home = os.path.join(self.dir, "jdtls")
        self.put("jdtls/plugins/org.eclipse.equinox.launcher_1.6.900.jar", "")
        self.put("jdtls/plugins/org.eclipse.equinox.launcher_1.7.0.jar", "")
        os.makedirs(os.path.join(home, launcher.config_name()))
        os.environ["TCODE_JDTLS_HOME"] = home
        try:
            self.assertEqual(launcher.find_jdtls(), os.path.realpath(home))
        finally:
            del os.environ["TCODE_JDTLS_HOME"]
        java = self.put("jdk/bin/java", "#!/bin/sh\n")
        os.chmod(java, 0o755)
        self.put("jdk/release", 'IMPLEMENTOR="x"\nJAVA_VERSION="25.0.5"\n')
        self.assertEqual(launcher.java_version(java), 25)
        self.put("jdk/release", 'JAVA_VERSION="1.8.0_402"\n')
        self.assertEqual(launcher.java_version(java), 8)

        argv = launcher.command(self.dir, home, "/usr/bin/java")
        self.assertEqual(argv[argv.index("-jar") + 1], os.path.join(home, "plugins/org.eclipse.equinox.launcher_1.7.0.jar"))
        self.assertEqual(argv[-1], launcher.workspace_dir(self.dir))
        self.assertIn("-Xmx1G", argv)
        self.assertFalse(any(a.startswith("-javaagent") for a in argv))
        self.assertNotEqual(launcher.workspace_dir(self.dir), launcher.workspace_dir(home))


if __name__ == "__main__":
    unittest.main()
