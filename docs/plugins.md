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
| `start` | string | no | `"project"`: start the plugin when a project opens and stop it when the project closes, for plugins that listen to events. Needs the `start` permission. Without it, the plugin runs only when one of its commands is run. |
| `permissions` | list | no | What the plugin does: `"edit"`, `"terminal"`, `"network"`, `"start"`. See [Permissions](#permissions). |
| `run` | object | no | Files that Run (`F5` / Run button) hands to one of the plugin's commands: `{ ".m": "matlab.run" }`. See [Declarations](#declarations-without-a-program). |
| `lsp` | list | no | Language servers the plugin adds. See [Declarations](#declarations-without-a-program). |
| `language-specs` | string | no | A folder inside the plugin's with GtkSourceView `.lang` files. |
| `snippets` | string | no | A folder inside the plugin's with GtkSourceView `.snippets` files. |
| `styles` | string | no, **(planned)** | A folder of style schemes. |
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
program. A broken `lsp` entry, `run` entry or folder is left out with a warning; the rest of the
plugin still loads.

### Declarations without a program

`lsp`, `run`, `language-specs` and `snippets` need no program of the plugin's own (only `run` names a
command, and commands need one). A plugin can be nothing but a `plugin.json` that declares a language
server:

```json
{
  "api": "tapi_1",
  "name": "MATLAB Language Server",
  "lsp": [
    {
      "argv": ["matlab-language-server", "--stdio"],
      "extensions": [".m"],
      "language-id": "matlab"
    }
  ]
}
```

Each `lsp` entry:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `argv` | list of strings | yes | The server's command line. `argv[0]` is a program found on `PATH` (and in `~/.local/bin`, `~/.cargo/bin`, `~/go/bin`), or a path relative to the plugin's folder (`bin/server`). The server speaks LSP over stdin / stdout and runs in the project's folder. |
| `extensions` | list of strings | one of the two | The files it serves: `".m"` matches names ending in it, anything else a whole name (`"CMakeLists.txt"`). |
| `languages` | list of strings | one of the two | Or by GtkSourceView language id (`"c"`, `"cpphdr"`, `"python"`): also files GtkSourceView recognizes by their content. |
| `name` | string | no | Unique within the plugin, shown in the status bar ("matlab-ls: starting"). Defaults to `argv[0]`'s file name. |
| `language-id` | string or object | no | The LSP `languageId` of its files. A string for all of them, or by file: `{ ".jsx": "javascriptreact", ".js": "javascript" }`. Defaults to GtkSourceView's id (`c`, `cpp`, `python`, `rust`), which is the LSP one for most languages. |
| `options` | object | no | Sent as `initializationOptions`. |
| `settings` | object | no | Sent with `workspace/didChangeConfiguration` after `initialize`, and when the server asks (`workspace/configuration`). |

- **One server per file.** A built-in server that is on and installed wins; turn it off in Settings →
  Language Servers to let a plugin's take its files. Where t-code has no built-in server (`.m`,
  `.tex`, `.lua`), or the built-in one is not installed, the first plugin that declares the file gets
  it.
- **Like a built-in one.** A plugin's server gets the same requests as the built-in ones: diagnostics,
  completion, hover, signature help, inlay hints, semantic tokens, symbols, rename, formatting. It
  starts with the first of its files, one per project, stops a while after its last file closes,
  and is restarted after a crash.
- **Not installed?** t-code does not offer to install a plugin's server. When its program is not
  found (on `PATH`, in `~/.local/bin`, `~/.cargo/bin`, `~/go/bin`, or in the plugin's folder for a
  relative path), its files quietly get no server, and the Plugins window shows the server as "not
  found". Say how to install it in the plugin's description or README.
- **Reloading** (Plugins: Reload Installed Plugins) moves the open files to the servers the plugins
  declare now.

`run` maps files (the same patterns as `extensions`) to one of the plugin's command ids. Run (`F5`,
the Run button) on such a file saves every editor, then runs that command with the file as `$2`, in
place of t-code's own runner. A project's `.t-code/tasks.json` still wins: it says what Run does.

`language-specs` and `snippets` name folders inside the plugin's folder. They are read when t-code
starts (GtkSourceView reads its folders once), so a plugin installed or reloaded later brings them at
the next start. t-code's own languages and the user's snippets come first.

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
   | `TCODE_PLUGIN_TOKEN` | A secret for `hello tapi_1 <token>`, new each time t-code loads its plugins. |
   | `TCODE_COMMAND_ID` | The command's id (also `$1`). |
   | `TCODE_ACTIVE_FILE` | The active editor's file, or empty (also `$2`). |
   | `TCODE_PROJECT_ROOT` | The project's folder. |

3. The program talks to t-code over the socket, then exits. Or it keeps a `subscribe` connection
   open and reacts to events.
4. Its stdout and stderr are read line by line: t-code keeps the last 200 lines per plugin (the
   Plugins window shows them) and copies them to its own stderr as `<id>: <line>`.
   When it exits with a non-zero status, the status bar says "Plugin <name> failed".

### Starting with the project

With `"start": "project"` (and `"start"` in `permissions`), t-code starts the program when a project
opens, with the command id `start` (`$1` and `$TCODE_COMMAND_ID`), the same environment as above and no
waiting. It is the place for a plugin that keeps a `subscribe` connection open.

- **Once per project:** opening another folder stops it and starts it again for the new one
  (`$TCODE_PROJECT_ROOT` changes). Without a folder open (t-code started in the home folder) it does
  not start. **Plugins: Reload Installed Plugins** restarts it with a new token.
- **Stopping:** t-code sends SIGTERM to the program's process group (a script's children go with
  it), then SIGKILL 2 seconds later if it is still there. When t-code quits it waits 1 second before
  SIGKILL. Exit on SIGTERM, or when the socket connection closes.
- **No restarts:** a start program that exits by itself is not started again. Exiting with status 0
  counts as done; anything else marks the plugin as failed and the status bar says so.
- A program that only runs commands does not need `start`: commands work whether it runs or not.

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
hello tapi_1 <token>
```

`<token>` is `$TCODE_PLUGIN_TOKEN` from the plugin's environment. It tells t-code which plugin is
speaking: the status items, diagnostics and terminal tabs it makes are its own, and its manifest's
[permissions](#permissions) apply to it. A script or a person (no plugin) says only `hello tapi_1`;
what it makes belongs to "socket", and no permissions are checked.

t-code answers `ok tapi_1 t-code <version>`. From then on the connection speaks tapi_1, and every
request gets one answer line. Otherwise:

- `error unsupported-api <message>`: t-code does not speak that version. The connection stays a
  legacy one.
- `error bad-argument …`: the token is not one t-code gave out (the plugins were reloaded meanwhile,
  for example). The connection stays a legacy one.

`hello` without a version picks the newest one t-code speaks.

### Answers

On a tapi_1 connection, every request gets exactly one answer line, in the order the requests came:

- `ok`: done.
- `ok <value>`: done, with a one-line value (`get-file`, `get-cursor`, `pick`, …).
- `error <code> <message>`: not done. `<code>` is one word a plugin can test, and it is part of
  the API. `<message>` is for people: it is written casually, sometimes jokingly, and may change in
  any version. Test the code, show the message.

`get-text` and `get-selection` answer `ok`, then the text, then a NUL byte, so text with line breaks
and empty text are both unambiguous.

`pick` and `ask` wait for the user. Until they are answered, t-code reads no further requests from
that connection, so the answers stay in order.

"Done" means t-code took the request. Some requests finish later: `save` writes the file in the
background, and `open` loads the file in the background. Listen for `did_save` / `did_open` when the
moment matters.

Error codes in tapi_1:

| Code | Meaning |
|---|---|
| `unknown-command` | No such command (the message names it). |
| `bad-argument` | The argument is not usable: not an absolute path, not a folder, bad JSON, no such terminal tab, an unknown token. |
| `no-editor` | The request needs an editor, and the active tab is not one. |
| `not-permitted` | The plugin's manifest does not declare the permission the request needs. |
| `canceled` | The user closed the picker without answering `pick` or `ask` (or another question took the picker). |
| `unsupported-api` | `hello` named a version this t-code does not speak. |

New codes may be added; treat an unknown code as a failure.

### Places, ranges and JSON

- Lines and columns are **1-based**. Columns count characters (not bytes, not UTF-16 units).
- A place past the end of a line is taken as the line's end, and a line past the end of the file as
  the file's end.
- A range is `[line, column, line, column]`: from the first place up to, not including, the second.
- Requests that take structured arguments take one line of JSON, such as `edit`, `pick` and
  `diagnostics-set`. Compact JSON has no raw line breaks, so these requests work with line framing.

### Commands

**Talking to t-code:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `hello` | `tapi_N [token]` or nothing | `ok tapi_N t-code <version>` | Starts tapi answers and names the plugin (above). |
| `version` | – | `ok tapi_1 t-code <version>` | The newest API version and t-code's version. Answers legacy connections too. |
| `subscribe` | – | `ok` | Turns the connection into an event stream (below). |

**The active editor:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `get-file` | – | `ok <path>` | The active editor's file; empty when the active tab is not an editor. |
| `get-project` | – | `ok <dir>` | The project's folder. |
| `get-text` | – | `ok`, then text and NUL | The whole buffer, unsaved edits included (empty without an editor). |
| `get-cursor` | – | `ok <line> <column>` | The cursor. |
| `get-selection` | – | `ok`, then text and NUL | The selected text (empty when nothing is selected). |
| `get-selection-range` | – | `ok <line> <col> <line> <col>` | Where the selection starts and ends (the start first, whichever end the cursor is at); the cursor twice when nothing is selected. For an `edit` that replaces the selection. |
| `set-selection` | `<line> <col> <line> <col>` | `ok` | Selects from the first place to the second; the cursor goes to the second. Equal places just move the cursor. |
| `insert` | text | `ok` | Inserts text at the cursor. Needs `edit`. |
| `append` | text | `ok` | Adds text at the end of the buffer. Needs `edit`. |
| `replace-all` | text | `ok` | Replaces the whole buffer. Needs `edit`. |
| `edit` | `[{"range":[l1,c1,l2,c2],"text":"…"}, …]` | `ok` | Replaces each range with its text, all as one undo step. Ranges are the text's before any edit and must not overlap. The cursor stays where it was. Needs `edit`. |
| `save` | – | `ok` | Saves the active editor. |

All of these except `get-file`, `get-project` and `get-text` answer `error no-editor` when the active
tab is not an editor.

**Files and folders** (the argument is an absolute path):

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `open` | `path[:line[:column]]` | `ok` | Opens a file in the active editor group, at a place when one is given. A file whose name really ends in `:12` is opened as it is. |
| `open-side` | path | `ok` | Opens a file in the right editor group. |
| `open-window` | path | `ok` | Opens a file in the second window. |
| `preview` | path | `ok` | Opens a Markdown preview tab. |
| `compare` | path | `ok` | Compares the file with its last commit, side by side. |
| `history` | path | `ok` | Lets the user pick a commit from the file's history to compare with. |
| `project` | dir | `ok` | Switches to another project folder. |
| `terminal` | dir | `ok` | `cd` to the folder in the terminal tab showing now. |

**Telling and asking the user:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `say` | text | `ok` | A short message in the status bar (the next status update replaces it). |
| `status-set` | `<id> <text>` | `ok` | A lasting item in the status bar, the plugin's own, named `id` (one word). Made the first time, changed after. |
| `status-clear` | `<id>` | `ok` | Removes that item. |
| `pick` | `{"title":"…","items":["a", {"label":"b","detail":"…"}]}` | `ok <index> <label>` | Shows the items in t-code's picker; the user types to filter. The answer is the chosen item's index (from 0) and label. |
| `ask` | question | `ok <text>` | Asks for one line of text in the picker. |
| `copy` | text | `ok` | Puts text on the clipboard. |
| `scm-message` | text | `ok` | Fills the Source Control panel's commit message box. |

`pick` and `ask` answer `error canceled …` when the user presses Escape or clicks
elsewhere. Only one question shows at a time: a newer one (from any plugin) cancels the older one.

**Diagnostics:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `diagnostics-set` | `{"path":"/abs/file","items":[…]}` | `ok` | The plugin's diagnostics for that file, in place of its earlier ones there. `[]` removes them. |
| `diagnostics-clear` | path, or nothing | `ok` | Removes the plugin's diagnostics for one file, or for every file. |

Each item is
`{"range":[line,col,line,col],"severity":"error|warning|info|hint","message":"…","code":"…","source":"…"}`.
Only `range` and `message` are required; `severity` defaults to `error`, and `range` may hold only the
start (`[line, col]`).

- **Where they show:** like a language server's diagnostics: underlined in the editor, counted in the
  status bar and listed in the Problems panel as `message (source code)`. Hints are not underlined.
- **Files not open yet:** t-code keeps the diagnostics per file. A file that is not open shows them
  when it opens.
- **Who replaces what:** a plugin's list never replaces a language server's, or another plugin's.
- **Moving with the text:** the marks move with the text the user types, until the plugin sends its
  next list.

**Terminal tabs:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `terminal-open` | `<title> [-- program args…]` | `ok <id>` | Opens a terminal tab in the project folder, running the program, or the user's shell without one. The program line is split like a shell would (quotes work). Needs `terminal`. |
| `terminal-send` | `<id> <text>` | `ok` | Types the text into the plugin's tab and presses Enter. |
| `terminal-interrupt` | `<id>` | `ok` | Sends Ctrl+C to the plugin's tab. |
| `terminal-close` | `<id>` | `ok` | Closes the plugin's tab; its program gets SIGHUP. |

- **Lifetime:** a plugin's tab closes by itself when its program exits.
- **Not restored:** it is not brought back with the project's other terminal tabs.
- **Ownership:** a plugin can only send to and close its own tabs.

### Events

Send `subscribe` and keep the connection open. t-code answers `ok` (a legacy connection gets
`{"event":"subscribed"}`), then sends one JSON object per line:

```json
{"event":"active_changed","path":"/absolute/path/to/file.c"}
{"event":"did_open","path":"/absolute/path/to/file.c"}
{"event":"did_change","path":"/absolute/path/to/file.c"}
{"event":"did_save","path":"/absolute/path/to/file.c","by":"plugin"}
{"event":"did_close","path":"/absolute/path/to/file.c"}
{"event":"project_changed","path":"/absolute/path/to/project"}
```

| Event | When |
|---|---|
| `did_open` | A file has loaded in an editor, including a new file that does not exist yet. Load failures other than "not found" do not send it. |
| `did_change` | A file's text changed, sent once typing pauses (300 ms). It carries only the path: read the text with `get-text`. |
| `did_save` | A file was written, autosaves included. |
| `did_close` | A file's editor tab closed. |
| `active_changed` | Another tab became the active one. The path is empty when the active tab is not an editor. |
| `project_changed` | Another project folder opened. |

`did_change` and `did_save` carry `"by":"plugin"` when a socket request caused them: an `insert`,
`edit` or `replace-all` (when every change since the last `did_change` came from the socket), or a
`save`. A formatter or a linter uses this to ignore the results of its own work.

New event names and new fields may appear: ignore what you do not know.

Read events as they come. t-code never waits for a subscriber; one that leaves more than 256 KB
unread is dropped and its connection closed. Closing the connection ends the subscription. When
nobody is subscribed, t-code does no event work at all.

### Legacy connections

A connection that never says `hello` is a legacy connection. t-code answers it exactly as before the
API had a version, so old plugins and scripts keep working:

- Only `get-file`, `get-project`, `get-cursor` (the bare value and a newline), `get-text`,
  `get-selection` (the text and a NUL), `version` and `subscribe` answer. Other requests get no
  answer, even when they fail; `pick` and `ask` still wait for the user.
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
  speaks. A plugin that wants them ships a language server and declares it in `lsp`.
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

### Permissions

A plugin declares what it does:

| Permission | Meaning | Enforced |
|---|---|---|
| `edit` | Changes buffers (`insert`, `append`, `replace-all`, `edit`). | Yes: without it, those requests answer `error not-permitted`. |
| `terminal` | Opens terminal tabs and starts programs in them. | Yes, for `terminal-open`. |
| `network` | Uses the network. | No: a declaration, checked by review. |
| `start` | Starts with the project (`"start"`). | Yes: without it, `"start"` is left out with a warning. |

- **Who is checked:** permissions apply to connections that said hello with the plugin's token. A
  plugin could skip the token and act as a script. Enforcement catches honest mistakes; trust comes
  from review.
- **Plugins from before versions:** a plugin without `"api"` is not checked.
- The Plugins window shows a plugin's permissions. **(planned)** Browse will show them before a
  plugin is installed.

### The Plugins window

**Plugins: Manage Plugins** (command palette, or Settings → Plugins… in the menu bar) opens a window: every plugin found on the left, the one
picked on the right.

- **What it shows:** name, description, id, version, API (`tapi_1`, or none for a plugin from
  before versions), where it was found (your plugins folder, the system, t-code itself), its folder
  and its state (on, off; for a `start` plugin: running, stopped, failed, not started). Then its
  commands with their keys, its permissions, what it brings (language servers, Run, language specs,
  snippets) and the last 200 lines its programs printed, which update while the window is open.
- **On / off:** the switch. Off, a plugin's commands and keys leave the palette, its language servers
  and Run stop being used, its start program stops and it does not start again. On, all of that comes
  back (it starts when a project is open). Its snippets and language specs follow at the next start.
  The choice is kept in `~/.config/t-code/plugins.json` (`"disabled"`: the ids of the plugins that are
  off).
- **Start / Stop:** for a `start` plugin, by hand (stopping is the same SIGTERM, then SIGKILL).
- **Open Folder**, and **Uninstall** for a plugin in your plugins folder: it stops, and its folder is
  deleted after a question. Plugins that came with the system or with t-code cannot be uninstalled
  here.
- **Reload** and **Open Plugins Folder** at the top: put a plugin's folder in
  `~/.config/t-code/plugins/`, press Reload, and it shows.
- **Browse:** the registry's plugins (below), fetched when the page shows (again after a minute, and
  on Reload), with a search over
  names, descriptions and categories. Each shows its version, its review badge and the permissions it
  asks for. **Install** downloads the pinned commit, checks its tree hash and its `plugin.json`'s id,
  and puts it in `~/.config/t-code/plugins/<id>/`; **Update** does the same when the registry pins a
  newer commit. A plugin that needs a newer t-code (`api`) cannot be installed. A reviewed plugin
  installs switched on; an unreviewed one installs switched off.
- A plugin installed from the registry shows "Registry: reviewed / not reviewed yet at <commit>" in
  its details; one put in the folder by hand shows "installed by hand" and never warns.

### The registry

Plugins are shared through the [t-code-plugins](https://github.com/suacayipkisi/t-code-plugins)
repository. It holds no plugin code, only pins; its README says how to publish.

- **One file per plugin.** `plugins/<id>.json` holds the plugin's repository on GitHub, a full
  commit id, the plugin's folder in it (`path`), the tree hash of that folder (`tree-sha256`), its
  name, description, version, API, permissions, categories and `"reviewed": false|true`.
- **The tree hash** is the sha256 of one line per regular file of the plugin's folder, sorted by
  path: `<file's sha256> <x or -> <path>` (`x`: executable). It does not depend on how GitHub
  compresses archives. A folder with symbolic links or special files cannot be installed.
- **Publishing.** An author opens a pull request adding or updating that file (and the generated
  `index.json`).
- **Automatic checks.** CI (`tools/check.py`) checks the pull request:
  - the entry's fields, and that only a maintainer turns `reviewed` on;
  - the commit: it downloads, its `plugin.json` matches the entry, its files match `tree-sha256`;
  - shell scripts with shellcheck;
  - no binaries (images aside), no symbolic links, no `curl | sh`, no network use without the
    `network` permission (talking to t-code's socket does not count);
  - a smoke test: each command, and `start`, runs against a fake t-code socket.
- **Merged unreviewed.** When the checks pass, the pull request is merged with `"reviewed": false`.
- **Maintainer review.** A t-code maintainer reads that exact commit and sets `"reviewed": true`.
  A new commit starts unreviewed again.
- **The index.** t-code reads `index.json` from the repository's main branch over HTTPS (curl);
  `$TCODE_REGISTRY` names another URL, or a file. It holds `{"format": 1, "plugins": [...]}`, the
  entries as above. **(planned, before 1.0)** a signature over the index, checked by t-code.

t-code warns before it runs an unreviewed plugin, three times:

1. when it is installed (it installs switched off);
2. when it is first turned on;
3. when it first runs (a command, or its start program).

After that it stops asking, until an update brings a new unreviewed commit. The badge stays until a
maintainer reviews it; t-code notices the review the next time Browse fetches the index. What was
shown is kept in `~/.config/t-code/plugins.json` (`"registry"`: per plugin, its repository, commit,
whether it was reviewed and the warnings taken).

---

## Plugin catalog

These are plugins the API is meant to make possible. Four of them are written and live in this
repository under `plugins/builtin/` (MATLAB, ShellCheck, CMake, Text Tools), as working examples and as the
registry's first entries; t-code does not load them by itself (install them from Browse, or copy a
folder into `~/.config/t-code/plugins/`). Each one says what it does, what t-code provides
(**core**), what the plugin itself does, and where it stops. Some still use **(planned)** parts of tapi_1.

### Languages and REPLs

#### MATLAB (`matlab`, in this repository: `plugins/builtin/matlab`)
- **Does:** write and run `.m` code without the MATLAB desktop: Run File (also `F5` on a `.m` file),
  Run Selection, Stop, Open / Close Session.
- **Core:**
  - Terminal tabs (`terminal-open`, `terminal-send`, `terminal-interrupt`, `terminal-close`).
  - `"run": {".m": "run"}`, so `F5` / Run on a `.m` file runs the plugin's command.
  - GtkSourceView's own `matlab` language (no language specs needed).
  - An `lsp` entry for the MATLAB Language Server (`matlab-language-server --stdio`, installed by
    the user; until then the Plugins window shows it as not found).
- **Plugin** (Python 3, standard library):
  - **Session:** a "MATLAB" terminal tab running `matlab -nodesktop -nosplash` (found on `PATH` or
    in `/usr/local/MATLAB/*`, `/opt/MATLAB/*`, `~/MATLAB/*`). Its tab id is kept in
    `$XDG_RUNTIME_DIR/t-code-matlab/`, so every command reaches the same session.
  - **Run:** `save`, then one line into the session: a `diary` file, the file run inside
    `try` / `catch` with `getReport`, and markers around it. The plugin waits for the end marker, turns
    `Error in foo (line 12)` and `Error: File: … Line: 3 Column: 5` into `diagnostics-set` (the
    first frame an error, the callers info), and clears them on the next run.
  - **Run Selection:** `get-selection`, then each line with `terminal-send`.
  - **Stop:** `terminal-interrupt` (Ctrl+C). It closes only its own tab.
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

#### ShellCheck (`shellcheck`, in this repository: `plugins/builtin/shellcheck`)
- **Does:** when a shell script is opened or saved (`.sh`, `.bash`, `.ksh`, `.bats`, or a `#!` line
  naming sh / bash / dash / ksh), runs `shellcheck -f json1 -x` and turns its findings into
  diagnostics, with codes like `SC2086` (ShellCheck's `style` level as hints). ShellCheck: Check This
  File does it once by hand.
- **Plugin** (Python 3): `"start": "project"`, a `subscribe` connection for `did_open`, `did_save`
  and `did_close`, and a second one for `diagnostics-set`. ShellCheck counts a tab as 8 columns; the
  plugin converts to characters.
- **Limits:**
  - Checks on open and save only.
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
- **Could:** keep a lasting `status-set` item, updated on `did_change`.

### Build, devices, containers

#### CMake (`cmake`, in this repository: `plugins/builtin/cmake`)
- **Does:** CMake: Configure, Build, Clean and Select Configure Preset, beyond the built-in Run (which
  always configures `build/` as Debug).
- **Core:**
  - `pick` for the presets, `diagnostics-set`, terminal tabs (`terminal-open`, `terminal-send`).
  - An `lsp` entry for cmake-language-server (`pip install cmake-language-server`). The built-in
    neocmakelsp wins when it is installed and on, so the plugin's server serves CMake files only
    without it (or with it turned off in Settings → Language Servers).
- **Plugin** (Python 3, standard library):
  - **Configure:** runs `cmake` itself (`--preset <name>`, else `-B build -DCMAKE_BUILD_TYPE=Debug
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`), turns `CMake Error at <file>:<line> (<command>):` and
    the warnings into diagnostics (the indented lines under them as the message, the call stack left
    out) and says the result. It remembers the build folder CMake reports.
  - **Presets:** `cmake --list-presets=configure` in the picker (includes and user presets
    resolved by CMake), plus "No preset"; choosing one configures with it.
  - **Build / Clean:** `cmake --build <folder>` (`--target clean`) typed into a "CMake" terminal tab,
    a shell that stays open after the build; it configures first when there is no `CMakeCache.txt`.
  - The preset, build folder and tab id are kept in `$XDG_RUNTIME_DIR/t-code-cmake/`, per project.
- **Limits:**
  - Build and test presets are not used: Build builds the configure preset's folder.
  - Compiler errors from Build stay in the terminal (clangd shows them in the editor).
  - No target picker yet.

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

#### Text tools (`text-tools`, in this repository: `plugins/builtin/text-tools`)
- **Does:** UPPER / lower / Title / snake_case / camelCase, sort (both ways), remove duplicate
  lines, reverse lines, trim trailing spaces, base64 and URL encode / decode, format / minify JSON,
  insert a UUID or the date and time.
- **Plugin** (Python 3): `get-selection-range` and `get-selection`, then one `edit` (one undo step).
  The line and JSON commands take the whole buffer when nothing is selected; the others need a
  selection.

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

### Built-in language servers as plugins

The language servers t-code knows (clangd, pylsp, gopls, rust-analyzer, typescript-language-server,
the JSON and YAML servers, marksman, neocmakelsp) can be turned off one by one in Settings → Language
Servers (`language-servers-off` in settings.ini). The same servers come back as plugins with only an
`lsp` entry: `tests/plugins/lsp-<server>/` in the repository holds one per server, with the same
command line, options and settings. They are tests, not shipped: the test suite turns the built-in
servers off and checks that each file goes to its plugin's server, started the same way. They are
also working examples: copy one into `~/.config/t-code/plugins/` to try it.

---

## Not planned

**Native modules** (`.so` files loaded into t-code) are not planned. Freezing t-code's internal
structures as an ABI would freeze t-code itself, and a crashing module would take the window down.
Everything a native module could offer should come either through the socket or through a language
server.
