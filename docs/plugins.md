# Writing plugins for t-code

A t-code plugin is a program: a shell script, a Python script, a Go or C binary, anything that can
read and write a UNIX socket. t-code starts it, and it talks back to t-code over the control socket.
Plugins never run inside the t-code process.

This document is the plugin API, **tapi_1**: the manifest (`plugin.json`), the socket protocol, the
environment, the rules that keep old plugins working, the limits of the model, and a catalog of
plugins the API is meant to make possible.

Parts marked **(planned)** are designed but not in t-code yet. They are listed so plugin authors can
see where the API goes. Everything without that mark works today.

- [Principles](#principles)
- [Quick start](#quick-start)
- [Versions and compatibility](#versions-and-compatibility)
- [The manifest: `plugin.json`](#the-manifest-pluginjson)
- [How a plugin runs](#how-a-plugin-runs)
- [The socket protocol](#the-socket-protocol)
- [What a plugin cannot do](#what-a-plugin-cannot-do)
- [Trust, review and the plugin registry](#trust-review-and-the-plugin-registry)
- [Plugin catalog](#plugin-catalog)

---

## Principles

1. **No cost when idle.** An installed plugin that is not running costs no memory and no CPU. t-code
   reads its `plugin.json` and nothing else.
2. **A crash stays in the plugin.** A plugin that crashes, hangs or floods the socket must never
   freeze or crash the window. t-code never waits for a plugin.
3. **No embedded runtimes.** t-code embeds no JavaScript or Python engine. A plugin brings its own
   interpreter, if it needs one.
4. **Any language.** Anything that reads and writes a stream socket can be a plugin. A plugin of
   ten shell lines is a normal plugin.

---

## Quick start

A plugin is a folder with a `plugin.json` and a program:

```
~/.config/t-code/plugins/hello/
  plugin.json
  run.sh
```

`plugin.json`:

```json
{
  "api": "tapi_1",
  "name": "Hello",
  "version": "1.0.0",
  "description": "Says hello in the status bar",
  "commands": [
    { "id": "hello.say", "name": "Hello: Say Hello", "keys": "<Control><Alt>h" }
  ]
}
```

`run.sh` (executable):

```sh
#!/bin/sh
# $1 is the command id, $2 the active file (empty when none)
printf 'hello tapi_1\nsay Hello from %s\n' "${2:-nowhere}" | nc -U "$TCODE_SOCKET" >/dev/null
```

Run **Plugins: Reload Installed Plugins** from the command palette (`Ctrl+Shift+P`). Then press
`Ctrl+Alt+H`, or pick "Hello: Say Hello" in the palette. The repository's `plugins/word-count/` is a
complete example.

---

## Versions and compatibility

The API has a version name: `tapi_1`, `tapi_2`, and so on.

- **Each version holds all the ones before it.** `tapi_2` will be `tapi_1` plus new commands, events
  and manifest fields. Nothing in a version is ever changed or removed by a later one. A plugin
  written for `tapi_1` keeps working on every t-code 1.x.
- **A plugin names the oldest version it needs.** `"api": "tapi_1"` means "I need tapi_1 or newer".
  A t-code that does not know the named version skips the plugin and says it needs a newer t-code.
- **Plugins from before versions.** A `plugin.json` without `"api"` is a plugin written before the API
  had a version. It loads and runs as before. Its socket connections are *legacy* connections (see
  [Legacy connections](#legacy-connections)).
- **Additions inside a version.** Until t-code 1.0, tapi_1 still grows: the **(planned)** parts below
  join it. From 1.0 on, tapi_1 is frozen and new things go into tapi_2.
- **Deprecated, never removed.** A field that is replaced keeps working. For example, `trigger` was
  replaced by `keys`: t-code still reads `trigger`, but new plugins should not use it.

A plugin can learn the version at run time in two ways:

- `$TCODE_API` in its environment holds the newest version this t-code speaks.
- The socket's `version` request answers the same, plus the t-code version.

---

## The manifest: `plugin.json`

### Where plugins are found

t-code looks in these folders, in this order. When two folders hold the same plugin id, the first one
found wins:

1. `~/.config/t-code/plugins/<folder>/`, created if missing. Plugins the user installs go here.
2. `/usr/share/t-code/plugins/<folder>/`. This exact path is used, whatever the install prefix.
   Distribution packages put plugins here.
3. `plugins/<folder>/` next to t-code's `styles/` folder. In the source tree, that is the
   repository's `plugins/`.

`make install` and t-code's packages install no plugins.

### Fields

| Field | Type | Required | Meaning |
|---|---|---|---|
| `api` | string | recommended | The oldest API version the plugin needs: `"tapi_1"`. Without it, the plugin is treated as one from before versions. |
| `name` | string | yes | The name people see. |
| `id` | string | no | A unique id. Defaults to the folder's name. Use lowercase, digits and `-`. |
| `version` | string | no | The plugin's own version, such as `"1.2.0"`. |
| `description` | string | no | One sentence. |
| `executable` | string | no | The program to run, relative to the plugin's folder or absolute. Without it, t-code looks for an executable `run.sh`, `plugin` or a file named like the folder. Only needed when the plugin has commands. |
| `commands` | list | no | The commands the plugin adds (below). A plugin without commands needs no program. |
| `start` | string | no, **(planned)** | `"project"`: start the plugin when a project opens and stop it when the project closes, for plugins that listen to events. Without it, the plugin runs only when one of its commands is run. |
| `permissions` | list | no, **(planned)** | What the plugin does: `"edit"`, `"terminal"`, `"network"`, `"start"`. See [Permissions](#permissions-planned). |
| `run` | object | no, **(planned)** | File extensions that Run (`F5` / Run button) hands to a plugin command, e.g. `{ ".m": "matlab.run" }`. |
| `lsp` | list | no, **(planned)** | Language servers the plugin adds: extensions, argv, root markers. |
| `language-specs` | string | no, **(planned)** | A folder of GtkSourceView `.lang` files. |
| `snippets`, `styles` | string | no, **(planned)** | Folders of snippets and style schemes. |
| `debuggers` | list | no, **(planned, maybe tapi_2)** | Debug adapters (DAP) the plugin adds: argv, languages, launch template. |

Each command:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `id` | string | yes | Unique within the plugin. Prefixing it with the plugin id (`hello.say`) is good practice. |
| `name` | string | yes | Shown in the command palette. Start with the plugin name: `"Hello: Say Hello"`. |
| `keys` | string | no | A shortcut in GTK's syntax: `<Control><Alt>h`, `<Shift>F7`, `<Control><Shift>m`. t-code makes the label people see (`Ctrl+Alt+H`) from it. |
| `trigger` | string | deprecated | The shortcut before tapi_1, when `keys` was only a label. Still read. Do not use it in new plugins. |

When two plugins ask for the same shortcut, the first plugin found keeps it. Invalid shortcuts are
left out, with a warning. A `keys` that GTK cannot read (such as `Ctrl+Alt+H`) is shown as a label
only, without a shortcut.

A plugin that is broken is skipped, with a warning on t-code's stderr. Broken means: no `name`,
`commands` that is not a list, an `api` this t-code does not speak, or commands but no executable
program.

---

## How a plugin runs

1. The user runs one of the plugin's commands, from the palette or with its shortcut.
2. t-code starts the plugin's program in the plugin's folder and does not wait for it. The program
   gets two arguments: the command id, and the active file (empty when the active tab is not an
   editor). Its environment holds:

   | Variable | Value |
   |---|---|
   | `TCODE_SOCKET` | The control socket's path. |
   | `TCODE_API` | The newest API version this t-code speaks, e.g. `tapi_1`. |
   | `TCODE_PLUGIN_ID` | The plugin's id. |
   | `TCODE_COMMAND_ID` | The command's id (also `$1`). |
   | `TCODE_ACTIVE_FILE` | The active editor's file, or empty (also `$2`). |
   | `TCODE_PROJECT_ROOT` | The project's folder. |

3. The program talks to t-code over the socket, then exits. Or it keeps a `subscribe` connection
   open and reacts to events.
4. Its stdout and stderr go to t-code's own stdout and stderr. **(planned)** The Plugins window
   shows them per plugin. When it exits with a non-zero status, the status bar says
   "Plugin <name> failed".

**(planned)** With `"start": "project"`, t-code starts the program when a project opens, with the
command id `start`, and stops it (SIGTERM, then SIGKILL) when the project closes or t-code quits.

---

## The socket protocol

### Connecting and framing

The socket is a UNIX stream socket at `$TCODE_SOCKET`. A plugin sends requests, one per line:

```
<command> <argument>\n
```

The argument is everything after the first space, and may be empty. When an argument can contain line
breaks (a buffer's text, an odd file name), end each request with a NUL byte instead of `\n`.
A connection that sends a NUL byte ends all its requests that way. Paths are always absolute.

"The active tab" is the current tab of the focused editor group.

### Saying hello

A connection should start with:

```
hello tapi_1
```

t-code answers `ok tapi_1 t-code <version>`. From then on the connection speaks tapi_1, and every
request gets one answer line. When t-code does not speak the version asked for, it answers
`error unsupported-api <message>` and the connection stays a legacy one. `hello` without a version
picks the newest one t-code speaks.

### Answers

On a tapi_1 connection, every request gets exactly one answer line, in the order the requests came:

- `ok`: done.
- `ok <value>`: done, with a one-line value (`get-file`, `get-project`, `version`).
- `error <code> <message>`: not done. `<code>` is one word a plugin can test. `<message>` is for
  people.

`get-text` answers `ok`, then the text, then a NUL byte, so text with line breaks, and an empty
buffer, are both unambiguous.

"Done" means t-code took the request. Some requests finish later: `save` writes the file in the
background, and `open` loads the file in the background. Listen for `did_save` / `did_open` when the
moment matters.

Error codes in tapi_1:

| Code | Meaning |
|---|---|
| `unknown-command` | No such command (the message is the command). |
| `bad-argument` | The argument is not usable: not an absolute path, not a folder, not a version. |
| `no-editor` | The request needs an editor, and the active tab is not one. |
| `unsupported-api` | `hello` named a version this t-code does not speak. |
| `not-permitted` | **(planned)** The request needs a permission the plugin's manifest does not declare. |

New codes may be added; treat an unknown code as a failure.

### Commands

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `hello` | `tapi_N` or nothing | `ok tapi_N t-code <version>` | Starts tapi answers (above). |
| `version` | – | `ok tapi_1 t-code <version>` | The newest API version and t-code's version. Answers legacy connections too. |
| `get-file` | – | `ok <path>` | The active editor's file; empty when the active tab is not an editor. |
| `get-project` | – | `ok <dir>` | The project's folder. |
| `get-text` | – | `ok`, then the text and a NUL | The active editor's whole buffer, unsaved edits included. |
| `insert` | text | `ok` | Inserts text at the cursor. |
| `append` | text | `ok` | Adds text at the end of the buffer. |
| `replace-all` | text | `ok` | Replaces the whole buffer. |
| `save` | – | `ok` | Saves the active editor. |
| `open` | path | `ok` | Opens a file in the active editor group. |
| `open-side` | path | `ok` | Opens a file in the right editor group. |
| `open-window` | path | `ok` | Opens a file in the second window. |
| `preview` | path | `ok` | Opens a Markdown preview tab. |
| `compare` | path | `ok` | Compares the file with its last commit, side by side. |
| `history` | path | `ok` | Lets the user pick a commit from the file's history to compare with. |
| `project` | dir | `ok` | Switches to another project folder. |
| `terminal` | dir | `ok` | `cd` to the folder in the shell. |
| `say` | text | `ok` | A short message in the status bar (the next status update replaces it). |
| `copy` | text | `ok` | Puts text on the clipboard. |
| `subscribe` | – | `ok` | Turns the connection into an event stream (below). |

**(planned for tapi_1, before 1.0):**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `open` | `path:line[:column]` | `ok` | Opens a file at a position. |
| `get-cursor` | – | `ok <line> <column>` | The cursor (1-based). |
| `get-selection` | – | `ok`, then text and NUL | The selected text. |
| `set-selection` | `<line> <col> <line> <col>` | `ok` | Selects a range. |
| `edit` | JSON: `[{"range":[l1,c1,l2,c2],"text":"…"}]` | `ok` | Applies edits as one undo step, keeping the cursor. Needs `"edit"`. |
| `status-set` | `<id> <text>` | `ok` | A lasting item in the status bar, owned by the plugin. |
| `status-clear` | `<id>` | `ok` | Removes it. |
| `pick` | `<title>`, then lines, then NUL | `ok <chosen line>` or `error canceled` | Shows a list in t-code's picker and answers the line the user chose. |
| `ask` | `<question>` | `ok <text>` or `error canceled` | Asks for one line of text. |
| `diagnostics-set` | `<path>`, then JSON, then NUL | `ok` | The plugin's diagnostics for a file: in the Problems panel and underlined in the editor. Replaces the plugin's earlier ones for that file. |
| `diagnostics-clear` | `[path]` | `ok` | Removes the plugin's diagnostics (for one file, or all). |
| `terminal-open` | `<title> -- <argv…>` | `ok <terminal id>` | Opens a terminal tab running a program. Needs `"terminal"`. |
| `terminal-send` | `<terminal id> <text>` | `ok` | Types text into the plugin's own terminal tab. |
| `terminal-close` | `<terminal id>` | `ok` | Closes a terminal tab the plugin opened (only its own). |
| `scm-message` | text | `ok` | Fills the Source Control panel's commit message box. |

The diagnostics JSON is a list of
`{"range":[line,col,line,col],"severity":"error|warning|info|hint","message":"…","code":"…","source":"…"}`.

### Events

Send `subscribe` and keep the connection open. t-code answers `ok` (a legacy connection gets
`{"event":"subscribed"}`), then sends one JSON object per line:

```json
{"event":"did_open","path":"/absolute/path/to/file.c"}
{"event":"did_save","path":"/absolute/path/to/file.c"}
```

- `did_open` is sent once a file has loaded, including a new file that does not exist yet. Load
  failures other than "not found" do not send it.
- `did_save` follows every successful write, autosaves included.
- **(planned)** `did_close`, `did_change` (after typing pauses; path only, read the text with
  `get-text`), `active_changed` (another tab became active), `project_changed`. Events caused by the
  plugin itself will carry `"by":"plugin"`, so a formatter can ignore its own saves.

New event names and new fields may appear: ignore what you do not know.

Read events as they come. t-code never waits for a subscriber; one that leaves more than 256 KB
unread is dropped and its connection closed. Closing the connection ends the subscription.

### Legacy connections

A connection that never says `hello` is a legacy connection. t-code answers it exactly as before the
API had a version, so old plugins and scripts keep working:

- Only `get-file`, `get-project` (the bare value and a newline), `get-text` (the text and a NUL),
  `version` and `subscribe` answer. Other requests get no answer, even when they fail.
- It may still use the internal names `debug` and `debug-failed`.

### Internal commands

`_debug <program>` and `_debug-failed <dir>` are how F5's build hands its program to the debugger
(through `t-code --send`). Commands starting with `_` are internal: they may change or disappear in
any version. Plugins must not use them.

---

## What a plugin cannot do

These limits follow from the process model. They are deliberate, not missing features.

- **No widgets of its own.** A plugin cannot draw GTK widgets, add panels or show web pages. It works
  through what t-code offers: commands, the status bar, the picker, diagnostics, terminal tabs, the
  buffer. In return, a broken plugin cannot break the window.
- **Not per keystroke.** A socket round trip takes milliseconds. Completion, inline suggestions,
  semantic highlighting and signature help belong in a **language server**, which t-code already
  speaks. A plugin that wants them ships a language server and declares it in `lsp` **(planned)**.
- **No drawing in the editor.** Beyond diagnostics (underlines, gutter marks, Problems), a plugin
  cannot decorate the text: no custom gutter icons, inline widgets or color boxes.
- **No hooks into deep parts.** Version control systems (git, Mercurial, Subversion; Bazaar later),
  test frameworks and the diff view are part of t-code itself. A plugin cannot add a new VCS or test
  framework. A test-provider protocol may come in a later version.
- **No remote files.** A plugin can open an `ssh` session in a terminal tab, but t-code edits local
  files only.
- **Sandboxed packages.** In the Flatpak and the Snap (strict confinement), plugins run inside the
  sandbox: they cannot see programs installed on the host (`matlab`, `shellcheck`, `latexmk`, …).
  Plugins that use host programs work fully in the distribution packages (Copr, PPA, AUR) and in
  source builds.
- **No security sandbox.** A plugin runs with the user's rights: it can read and write any file and
  use the network. Permissions are declarations, checked by review (see below), not a jail.
- **Linux only.** Like t-code.

---

## Trust, review and the plugin registry

### Permissions (planned)

A plugin declares what it does:

| Permission | Meaning | Enforced |
|---|---|---|
| `edit` | Changes buffers (`insert`, `append`, `replace-all`, `edit`). | Yes: without it, those requests answer `error not-permitted`. |
| `terminal` | Opens terminal tabs and starts programs. | Yes, for `terminal-open`. |
| `network` | Uses the network. | No: a declaration, checked by review. |
| `start` | Starts with the project (`"start"`). | Yes. |

The Plugins window shows them before a plugin is installed and enabled.

### The Plugins window (planned)

**Plugins: Manage Plugins** opens a window with two pages.

- **Installed:**
  - Every plugin, with its version, API, source (user, system, registry), state (running, stopped,
    failed, needs a newer t-code) and review badge.
  - Per plugin: Enable/Disable, Start/Stop, the commands and their keys, the permissions, the last
    lines of its output, Open Folder, Update, Uninstall (user folder only).
  - A disabled plugin's commands and keys are gone and it does not start.
- **Browse:**
  - The registry's plugins, with search and categories.
  - Install downloads the reviewed commit into `~/.config/t-code/plugins/<id>/` and checks its
    sha256.

### The registry (planned)

Plugins are shared through the `t-code-plugins` repository. It holds no plugin code, only pins:

- **One file per plugin.** `plugins/<id>.json` holds the plugin's source repository, a commit, the
  sha256 of that commit's archive, its API, its permissions and `"reviewed": false|true`.
- **Publishing.** To publish, an author opens a pull request adding or updating that file.
- **Automatic checks.** An automatic reviewer (CI) checks the pull request:
  - the manifest, and that the commit's archive matches the sha256;
  - shell scripts with shellcheck;
  - no binaries, no `curl | sh`, no undeclared network use;
  - a smoke test against a fake t-code socket.
- **Merged unreviewed.** When the checks pass, the pull request is merged with `"reviewed": false`.
- **Maintainer review.** A t-code maintainer reads that exact commit and sets `"reviewed": true`.
  A new commit starts unreviewed again.

t-code warns before it runs an unreviewed plugin, three times:

1. when it is installed;
2. when it is first enabled;
3. when it first runs.

After that it stops asking. The badge stays until a maintainer reviews it. The registry's index is
signed, and t-code checks the signature.

---

## Plugin catalog

These are plugins the API is meant to make possible. Each one says what it does, what t-code provides
(**core**), what the plugin itself does, and where it stops. Most use **(planned)** parts of tapi_1.

### Languages and REPLs

#### MATLAB (`matlab`)
- **Does:** write, run and send `.m` code without the MATLAB desktop.
- **Core:**
  - Terminal tabs (`terminal-open`, `terminal-send`).
  - `"run": {".m": "matlab.run"}`, so F5 / Run on a `.m` file runs the plugin's command.
  - The plugin's `language-specs/matlab.lang`.
  - An `lsp` entry for the MATLAB Language Server, installed by the user (completion, hover,
    diagnostics).
- **Plugin:**
  - **Session:** on its first command, opens a "MATLAB" terminal tab running
    `matlab -nodesktop -nosplash`. The plugin owns this session.
  - **Run:** `save`, then sends `run('/path/file.m')` to the tab. Send Selection: `get-selection`,
    then `terminal-send`.
  - **Errors as diagnostics:** MATLAB's messages show in the terminal tab. The plugin also has MATLAB
    write a `diary` file, watches it, turns lines like `Error in foo (line 12)` into
    `diagnostics-set`, and clears them on the next run.
  - **Stop:** sends Ctrl+C. It closes only its own processes.
  - **Alternative:** a Python plugin could attach to a shared session through `matlab.engine` and
    catch errors directly.
- **Limits:**
  - No workspace or variables panel; a variable list could show through `pick`.
  - Plot windows are MATLAB's own.
  - Needs the user's licensed MATLAB.
  - Not visible from inside the Flatpak.

#### Octave (`octave`)
- **Does:** the same as MATLAB, with `octave --no-gui`. Shares most of the MATLAB plugin's code.
- **Limits:** no language server; diagnostics come only from run errors.

#### Julia (`julia`)
- **Does:** a `julia` REPL in a terminal tab. Run sends `include("file.jl")`; Send Selection sends
  the selection.
- **Core:** an `lsp` entry for LanguageServer.jl.
- **Limits:** hot reloading (Revise.jl) is up to the user.

#### R (`r`)
- **Does:** an R REPL. Run sends `source("file.R")`. It can also render R Markdown / Quarto with
  `quarto render` and open the HTML with `xdg-open`.
- **Core:** an `lsp` entry for the `languageserver` package.
- **Limits:** plots open in their own windows.

#### Python REPL / IPython (`python-repl`)
- **Does:** sends the selection, or a `# %%` cell, to IPython in a terminal tab.
- **Plugin:** finds cell boundaries itself, with `get-text` and `get-cursor`.
- **Limits:** no `.ipynb` notebook editing: notebooks need a panel.

#### Haskell, OCaml, Elixir, Clojure REPLs
The same pattern each: a terminal tab, send, a `run` mapping and an `lsp` entry. Each is about 50–100
lines of script.

### Linters and formatters
All of these use the same pieces: `"start": "project"`, `subscribe`, `did_save` and
`diagnostics-set`.

#### ShellCheck (`shellcheck`)
- **Does:** on `did_save` of a `.sh` / `.bash` file, runs `shellcheck -f json` and turns its findings
  into diagnostics, with codes like `SC2086`.
- **Limits:**
  - Checks only on save.
  - Checking while typing needs `did_change` and `get-text` (the unsaved text) instead of the file.

#### Ruff / Flake8 / mypy (`python-lint`)
- **Does:** fast Python linting for people who do not use pylsp's.
- **Plugin:** Fix All runs `ruff --fix` on the text and applies the result with `edit`.

#### cppcheck / project-wide clang-tidy (`cppcheck`)
- **Does:** clangd checks one file at a time. Analyze Project runs over the whole project and puts
  the findings in Problems.
- **Plugin:** shows progress with `status-set` (`cppcheck: 40%`).

#### markdownlint, yamllint, hadolint, actionlint
Small plugins of the same shape, for Markdown, YAML, Dockerfiles and GitHub workflows.

#### Format on save (`format-on-save`)
- **Does:** prettier, black, shfmt, stylua, or rustfmt where no language server formats.
- **Plugin:** on `did_save`, formats and applies the difference with `edit`, so the cursor stays and
  undo is one step. It ignores events marked `"by":"plugin"`, so its own save does not loop.

#### Spell check (`spell`)
- **Does:** checks comments, strings and Markdown with `hunspell`. Findings are `hint` diagnostics.
- **Plugin:** Fix Spelling offers suggestions with `pick` and writes the chosen one with `edit`.
- **Limits:** suggestions cannot appear in the right-click menu.

### Documents

#### LaTeX (`latex`)
- **Does:** on `did_save`, runs `latexmk -pdf -interaction=nonstopmode`.
- **Plugin:**
  - Turns `! Undefined control sequence` / `l.42` lines from the `.log` into diagnostics.
  - View PDF opens zathura or okular.
  - Forward Search runs `zathura --synctex-forward line:col:file`.
  - Inverse search (PDF → code): set zathura's `--synctex-editor-command` to
    `t-code --send "open %{input}:%{line}"`.
- **Core:** an `lsp` entry for texlab.
- **Limits:** the PDF cannot show inside t-code.

#### Typst (`typst`)
- **Does:** keeps `typst watch` running and turns its errors into diagnostics.
- **Core:** an `lsp` entry for tinymist.

#### Diagrams (`diagrams`)
- **Does:** renders a Mermaid, PlantUML or Graphviz file or selection to SVG (`mmdc`, `plantuml`,
  `dot`) and opens it with `xdg-open`.
- **Limits:** cannot draw inside t-code's Markdown preview; the preview is part of t-code.

#### Word count (`word-count`, in this repository)
- **Does:** today, says the count in the status bar.
- **(planned)** A lasting `status-set` item, updated on `did_change`.

### Build, devices, containers

#### PlatformIO / Arduino (`platformio`)
- **Does:** Build, Upload and Serial Monitor.
- **Plugin:**
  - Build / Upload run `pio run -t upload` in a terminal tab; the monitor runs
    `pio device monitor` in another.
  - Compile errors become diagnostics.
  - Picks the board environment from `platformio.ini` with `pick`.
- **Core:** debugging through t-code's DAP needs a `debuggers` entry.
- **Limits:** no USB access inside the Flatpak or the Snap.

#### OpenOCD (`openocd`)
- **Does:** starts OpenOCD in a terminal tab and gives t-code's gdb a
  `target extended-remote :3333` launch.

#### Make / just / Task targets (`targets`)
- **Does:** lists the targets of a Makefile, justfile or Taskfile with `pick` and runs the chosen one
  in a terminal tab.

#### Meson, Bazel, Zig build
- **Does:** build commands, with their errors as diagnostics.
- **Core:** `compile_commands.json` already reaches clangd through t-code.

#### Docker / Podman (`containers`)
- **Does:** lists containers with `pick`. Build Image, Run, Logs (a terminal tab) and Exec Shell
  (`podman exec -it` in a terminal tab).
- **Limits:** no container tree in a sidebar.

#### SSH hosts (`ssh-hosts`)
- **Does:** lists the hosts of `~/.ssh/config` with `pick` and opens the chosen one in a terminal
  tab.
- **Limits:** no remote file editing.

### Debug adapters (declarations only, no program)

#### debugpy, delve, codelldb, lldb-dap
- **Does:** each plugin declares its adapter with a `debuggers` entry: argv, languages and a launch
  template. t-code already speaks DAP, so other adapters than gdb arrive this way.
- **Core:** t-code's debugger still holds some gdb-only assumptions that must be lifted first. This
  may land in tapi_2.

### Git and forges

#### GitHub / GitLab (`forge`)
- **Does:** uses the `gh` or `glab` command-line tools.
  - Open on GitHub: the line's link, opened with `xdg-open`.
  - Copy Permalink: through `copy`.
  - Check out PR: the open PRs in `pick`, then switch to the chosen PR's branch.
  - Create PR: runs `gh pr create` in a terminal tab.
- **Limits:** no review UI or comment panel.

#### Commit message helper (`commit-msg`)
- **Does:** fills Source Control's message box with `scm-message`, from a conventional-commit
  template or a language model.
- **Limits:** the user still commits.

### Everyday tools

#### TODO finder (`todos`)
- **Does:** finds TODO / FIXME with `rg`, then lists them with `pick` or as `info` diagnostics.

#### Text tools (`text-tools`)
- **Does:** change case, sort / unique lines, base64, URL encoding, JSON pretty printing, insert a
  UUID or a date.
- **Plugin:** `get-selection`, then `edit`.

#### Snippet packs and themes
Declarations only: `snippets/` and `styles/*.xml` folders, no program.

#### Time tracking (`wakatime`)
- **Does:** passes `active_changed` and `did_save` to `wakatime-cli`.
- **Permissions:** declares `network`.

#### Pomodoro (`pomodoro`)
- **Does:** a countdown with `status-set`, and `notify-send` at the end.

#### Discord presence (`discord`)
- **Does:** writes "Editing foo.c" to Discord's IPC socket on `active_changed`.

### Language models

#### Assistant (`llm`, with ollama or a hosted API)
- **Does:**
  - Explain Selection: the answer opens in a new Markdown tab.
  - Refactor Selection: through `edit`.
  - Write Docstring.
- **Permissions:** declares `network` and `edit`. The plugin handles its own keys and endpoints.
- **Limits:** inline suggestions (ghost text) cannot come through the socket. A plugin that wants
  them ships a language server that answers completion requests, declared in `lsp`.

### Built-in language servers as plugins (planned)

The language servers t-code knows (clangd, pylsp, gopls, rust-analyzer, …) can be turned off one by
one in Settings, and the same servers can come back as plugins with only an `lsp` entry. The
repository holds such plugins as tests. They show that a plugin's server works exactly like a built-in
one, and they are working examples for authors.

---

## Not planned

**Native modules** (`.so` files loaded into t-code) are not planned. Freezing t-code's internal
structures as an ABI would freeze t-code itself, and a crashing module would take the window down.
Everything a native module could offer should come either through the socket or through a language
server.
