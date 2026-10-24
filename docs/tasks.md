# Project Tasks & Launch Profiles

`t-code` supports configuring custom build, run, test and debug tasks via `.t-code/tasks.json` or `t-code.json` in your project root.

t-code looks for the file from the current file's folder upwards (the first folder that has one
wins; `.t-code/tasks.json` before `t-code.json`). When it is found, it takes precedence over every
other detection: CMake, Meson, Make, other languages' projects (Cargo, Node, ...) and single
scripts. The status bar's Build button then reads **Task: <profile>**. A file that cannot be read
or has none of `build`, `run`, `debug`, `test` makes Build and Run print a message in the shell.

## Quick Example: Single Configuration

```json
{
  "build": "make -j4",
  "run": "./build/t-code",
  "debug": "./build/t-code",
  "args": ["src/main.c"],
  "env": {
    "G_MESSAGES_DEBUG": "all",
    "TCODE_DEV": "1"
  },
  "cwd": "."
}
```

## Multi-Profile Configuration

You can define multiple named profiles (e.g. `debug`, `release`, `asan`) and select the active one with `"default"`:

```json
{
  "default": "debug",
  "profiles": {
    "debug": {
      "build": "cmake --build build --config Debug",
      "run": "build/bin/myapp",
      "debug": "build/bin/myapp",
      "test": "ctest --test-dir build --output-on-failure",
      "args": ["--port", "8080"]
    },
    "release": {
      "build": "cmake --build build --config Release",
      "run": "build/bin/myapp",
      "args": ["--production"]
    }
  }
}
```

The **Test** command (command palette) saves changed files and runs this profile's `test` command in the shell. When no tasks file applies, t-code detects the project's test convention: CMake/CTest, Meson, `make test`, Cargo, Go, a Node package's `test` script, Maven, Gradle, or `pytest` for a Python project. The command's output stays in the integrated shell; Ctrl-click a `file:line:column` location to open the failing source.

## Fields

| Field | Type | Description |
|---|---|---|
| `build` | string | Shell command executed on Build (`Ctrl+Shift+B`), and before Run and F5 |
| `run` | string | Command or executable launched on Run (`Ctrl+F5`) |
| `debug` | string | Path of the program handed to GDB on Debug (`F5`); `run` when missing. A program path, not a shell command |
| `args` | array or string | Arguments added after `run` (e.g. `["--verbose", "-n"]`; array items are shell-quoted, a string goes in as written). Not passed to the debugger |
| `env` | object | Environment variables for `run` and `test` (e.g. `{"PORT": "3000"}`); not for `build` or the debugger |
| `test` | string | Shell command executed by the **Test** command |
| `cwd` | string | Working directory for every command, relative to the folder holding the tasks file (or absolute) |

With `profiles`, `default` names the profile in use; without it (or when it names no profile)
the first profile is used. Fields outside `profiles` are used only when no profile applies (or the chosen one has none of
`build`, `run`, `debug`, `test`).

## Shortcuts

- `Ctrl+Shift+B`: Runs the `build` task.
- `Ctrl+F5`: Builds (if configured) and executes `run` with arguments, environment, and working directory in the integrated shell.
- `F5`: Builds and launches the debugger on `debug` (or `run`).
- Command palette **Test**: runs `test`.

There is no task picker yet: Build, Run, Test and F5 always use the profile named by `default`;
change `default` to switch profiles.
