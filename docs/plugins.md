# Writing plugins for t-code

A t-code plugin is a program: a shell script, a Python script, a Go or C binary, anything that can
read and write a UNIX socket. t-code starts it, and it talks back to t-code over the control socket.
Plugins never run inside the t-code process.

This document is the plugin API, **tapi_1**: the manifest (`plugin.json`), the socket protocol, the
environment, the rules that keep old plugins working, the limits of the model, and a catalog of
plugins the API is meant to make possible.

Everything described here works today: tapi_1 has no planned parts left before t-code 1.0.

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
  "provider": "Jane Doe",
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
`Ctrl+Alt+H`, or pick "Hello: Say Hello" in the palette. The repository's
`plugins/builtin/word-count/` is a small complete example of a plugin that starts with the project,
listens to events and keeps a `status-set` item up to date.

### A compiled language

The registry takes no binaries (reviewers read source), so a plugin in Rust, Go, C or Zig ships its
source and a small `run.sh` as its `executable`, which builds the program the first time and then
runs it:

```sh
#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
target="${XDG_CACHE_HOME:-$HOME/.cache}/t-code-<id>"
cargo build --release --offline --locked --quiet --manifest-path "$here/Cargo.toml" --target-dir "$target" >&2
exec "$target/release/<program>" "$@"
```

- Build into the cache, not the plugin's folder: the folder is what the registry's tree hash covers.
- With no dependencies (or vendored ones) the build needs no network; otherwise declare `network`.
- Ship the lock file (`Cargo.lock`, `go.sum`) so the build never writes next to the source.
- When the compiler is missing, say so on stderr and exit 1: the Plugins window shows the line.

`plugins/builtin/todos/` is such a plugin: Rust's standard library only, a `UnixStream` that says
`hello`, sends NUL-ended requests and reads one answer line each. `plugins/builtin/word-count/` is
another, in C++: its `run.sh` compiles one file with `c++` and names the program after the source's
`cksum`, so an update builds again.

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
- **Additions inside a version.** Until t-code 1.0, tapi_1 may still grow. From 1.0 on, tapi_1 is
  frozen and new things go into tapi_2.
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
| `provider` | string | no | Who publishes it: a person, a team or a project. The Plugins window shows it next to the plugin (Installed, Browse, the warnings). A plugin from the registry without one shows its repository's owner. |
| `id` | string | no | A unique id. Defaults to the folder's name. Use lowercase, digits and `-`. |
| `version` | string | no | The plugin's own version, such as `"1.2.0"`. |
| `description` | string | no | One sentence. |
| `executable` | string | no | The program to run, relative to the plugin's folder or absolute. Without it, t-code looks for an executable `run.sh`, `plugin` or a file named like the folder. Only needed when the plugin has commands. |
| `commands` | list | no | The commands the plugin adds (below). A plugin without commands needs no program. |
| `start` | string | no | `"project"`: start the plugin when a project opens and stop it when the project closes, for plugins that listen to events. Needs the `start` permission. Without it, the plugin runs only when one of its commands is run. |
| `permissions` | list | no | What the plugin does: `"edit"`, `"terminal"`, `"network"`, `"start"`. See [Permissions](#permissions). |
| `run` | object | no | Files that Run (`Ctrl+F5` / Run button) hands to one of the plugin's commands: `{ ".m": "matlab.run" }`. See [Declarations](#declarations-without-a-program). |
| `debug` | object | no | Files that Debug (`F5`) hands to one of the plugin's commands, which starts its debugger with `debug-start`: `{ ".java": "debug" }`. The same patterns as `run`. |
| `lsp` | list | no | Language servers the plugin adds. See [Declarations](#declarations-without-a-program). |
| `language-specs` | string | no | A folder inside the plugin's with GtkSourceView `.lang` files. |
| `snippets` | string | no | A folder inside the plugin's with GtkSourceView `.snippets` files. |
| `styles` | string | no | A folder inside the plugin's with GtkSourceView style schemes (`.xml`): each one is a color theme. See [Color themes](#color-themes). |
| `debuggers` | – | – | Not a manifest field: a plugin's debugger starts at run time with `debug-start` (its own command line, or a port where it listens), from the command `debug` names. |

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

`lsp`, `run`, `language-specs`, `snippets` and `styles` need no program of the plugin's own (only `run` names a
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
| `uri-contents` | object | no | For places the server gives in a URI scheme other than `file`: the request that reads such a document's text, by scheme: `{ "jdt": "java/classFileContents" }`. The request gets `{"uri": …}` and answers a string. Go to Definition into such a place saves the text in `~/.cache/t-code/sources/` (with the asking file's extension) and opens it read-only; the server hears of that file by its own URI, so hover and Go to Definition work inside it too. A place at its very start (jdtls' for a class without source) goes to the declaration of the type the file is named after. |

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

`run` maps files (the same patterns as `extensions`) to one of the plugin's command ids. Run (`Ctrl+F5`,
the Run button) on such a file saves every editor, then runs that command with the file as `$2`, in
place of t-code's own runner. A project's `.t-code/tasks.json` still wins: it says what Run does.
A `.m` file t-code takes for Objective-C never goes to a plugin's `run`. It does so when the text has
`#import`, `@interface` or `@implementation` at a line's start, or (with no `%` comment or `function`
line) a `.mm` file or a header of the same name sits next to it. Such a file is colored as
Objective-C, so a server declared for the `matlab` language does not get it either.

`language-specs` and `snippets` name folders inside the plugin's folder. They are read when t-code
starts (GtkSourceView reads its folders once), so a plugin installed or reloaded later brings them at
the next start. t-code's own languages and the user's snippets come first.

#### Color themes

Each GtkSourceView style scheme in the `styles` folder is a color theme, listed after t-code's own in
"Preferences: Color Theme…" and in Settings, by the scheme's `name`; its `id` is the theme's id (one
already taken, by t-code's themes or another plugin's, is left out with a warning). Unlike language
specs, themes follow the plugin at once: on Reload, and when it is switched on or off. Switched off
while its theme is shown, t-code shows Dark Classic, and the plugin's theme again when it comes back.

The editors use the scheme as it is. The rest of t-code (panels, tabs, the terminal, the diff gutter)
is worked out from it:

- the background and text from its `text` style; a light background makes a light theme;
- the code's colors from `def:comment`, `def:keyword`, `def:statement`, `def:type`, `def:function`,
  `def:identifier`, `def:constant`, `def:string`, `def:number`, `def:special-char` and
  `def:operator` (one it does not set: the text's color);
- the editor's selection, current line, line numbers, cursor, search matches and bracket match from
  the styles of those names;
- the accent (buttons, the current tab's line) from `def:keyword`.

Its `<metadata>` can say more, each a `<property>` t-code reads and GtkSourceView ignores:

| Property | Meaning |
|---|---|
| `t-code-accent` | The accent, instead of the keywords' color. |
| `t-code-frame` | The title bar, the status bar and the gaps between panels (else a little darker than the background). |
| `t-code-terminal` | The terminal's 16 colors, separated by spaces (else t-code's own, light or dark). |
| `t-code-<color>` | Any one of t-code's colors as it is, by its CSS name: `t-code-link`, `t-code-selection`, `t-code-git-modified`, ... (the `--tc-<color>` variables of `theme.css`). |

```xml
<style-scheme id="oak" name="Oak" version="1.0">
  <metadata>
    <property name="t-code-accent">#ff8800</property>
  </metadata>
  <style name="text" foreground="#e0d0c0" background="#302010"/>
  <style name="selection" background="#605040"/>
  <style name="def:comment" foreground="#889988" italic="true"/>
  <style name="def:keyword" foreground="#cc6633" bold="true"/>
</style-scheme>
```

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
   | `TCODE_TEST` | Only for `debug` run from the Testing panel: the test's id (`$2` is then its file). |
   | `TCODE_DEBUG_PORT` | With `TCODE_TEST`: the port where the test's JVM waits for a debugger. |

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

**Language servers:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `lsp-refresh` | – | `ok` | Looks again for the open files' language servers: a plugin that just installed its server's program uses it, so the files already open get the server without a reload. |
| `lsp-restart` | – | `ok` | Restarts every language server with the open files (Restart Language Servers): for something a server reads only when it starts, such as a bundle jdtls loads. |
| `lsp-command` | `{"command":"…","arguments":[…],"path":"/abs/file"}` | `ok <result as JSON>` | `workspace/executeCommand` on the language server of the editor showing `path` (without `path`: the active editor's). The result comes on one line, `null` too. `error no-editor` when the file is not open, `error no-server` while it has no server (yet), `error failed` when the server answers with an error. |

**Debugging:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `debug-start` | `{"name":"Java","connect":"127.0.0.1:5005","request":"launch","arguments":{…}}` | `ok` | Starts a debugging session with the plugin's debugger: `connect` is `host:port` where it listens (TCP), or `argv` its command line (DAP on stdin / stdout, run in `cwd`, an absolute path, the project's folder without one). `request` is `launch` (the default) or `attach`, `arguments` its arguments as the debugger wants them. `adapter-id` is the `adapterID` of `initialize` (default `plugin`), `name` shows in the Debug panel and `shown` after "▶" in its terminal. `error busy` while a session runs. |

- **Like a built-in one:** the session gets the breakpoints (conditions and logpoints too), the stack,
  variables, watch, stepping and the evaluate line, like gdb's. The debugger may start the program
  in the Debug panel's terminal (`runInTerminal`), where it reads and writes; an `uncaught` exception
  filter is turned on when the debugger has one.
- **From F5:** a file the plugin's `debug` names gets that command on `F5` (the editors saved first);
  the command prepares (builds, finds its port) and ends with `debug-start`. Restart runs it again.
- **From the Testing panel:** a JUnit test whose file the plugin's `debug` names gets a debug button
  (and Debug on its ▶ in the gutter). Its file opens, the editors are saved, and the panel runs the
  test itself (Maven's `maven.surefire.debug`, an init script's `jvmArgs` for Gradle) with its JVM
  waiting for a debugger on a free port (JDWP). Once that port listens (read from `/proc/net/tcp`),
  the same command runs with the file as `$2`, the test's id in `TCODE_TEST`
  (`com.example.CalcTest#adds`, a nested class's `Outer$Inner#name`) and the port in
  `TCODE_DEBUG_PORT`: it attaches its debugger there with `debug-start` (`"request": "attach"`). The
  test's result shows in the panel as a Run's would, and Restart stops the run and debugs the test
  again.

**Terminal tabs:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `terminal-open` | `<title> [-- program args…]` | `ok <id>` | Opens a terminal tab in the project folder, running the program, or the user's shell without one. The program line is split like a shell would (quotes work). Needs `terminal`. |
| `terminal-send` | `<id> <text>` | `ok` | Types the text into the plugin's tab and presses Enter. |
| `terminal-interrupt` | `<id>` | `ok` | Sends Ctrl+C to the plugin's tab. |
| `terminal-close` | `<id>` | `ok` | Closes the plugin's tab; its program gets SIGHUP. |
| `terminal-close-text` | `<id> [text]` | `ok` | The text typed (with Enter) into the plugin's tab when t-code closes, like `exit`; no text: nothing. |

- **Lifetime:** a plugin's tab closes by itself when its program exits.
- **When t-code closes:** the window hides and each plugin tab whose program still runs gets its close
  text. After up to 10 seconds (at once without a close text), the program and its helpers get SIGHUP
  and SIGTERM, and SIGKILL 2 seconds later. Helpers are the processes in the program's session
  (also those in their own process group) and its descendants (also those that started a new
  session). Nothing is ever ended by name, so the same program started elsewhere keeps running. A
  helper that leaves the session and its parent too (a double fork) is not found.
- **A close text matters:** some programs take an end by signal for a crash. MATLAB does: its crash
  reporter (double-forked, so t-code cannot find it) writes a dump and stays. After `exit` it goes.
- **Not restored:** it is not brought back with the project's other terminal tabs.
- **Ownership:** a plugin can only send to and close its own tabs.

**Views:**

| Command | Argument | Answer | What it does |
|---|---|---|---|
| `view-open` | `{"id":"…","title":"…","where":"bottom","icon":"…","text":"…"}` | `ok` | Opens the plugin's view `id` (one word without `/`): a read-only text panel in a dock (`where`: `left`, `bottom` (the default) or `right`) or a tab in an editor group (`editor`: the active one, `side`: the one beside it). `title` names its tab (the id without one), `icon` an icon name for a dock's tab. A view already open gets the title and text and shows. |
| `view-set` | `<id> <text>` | `ok` | Replaces the view's text. |
| `view-append` | `<id> <text>` | `ok` | Adds a line to the view's text and scrolls to it. |
| `view-show` | `<id>` | `ok` | Shows the view: its dock or tab, switched to. |
| `view-close` | `<id>` | `ok` | Closes the view. |

- **Its own:** a plugin reaches only its own views; another plugin's view of the same id is another
  view. `view-set`, `view-append`, `view-show` and `view-close` answer `error no-view` when the
  plugin has no such view open.
- **Moving:** a view in a dock is dragged and split like t-code's panels; a view tab like any tab.
- **Lines:** a double click or Enter on a line sends `view_activated` (below): a list of results opens
  what a line names.
- **Lifetime:** a view stays until the plugin closes it or the user closes its tab (`view_closed`);
  it is not brought back with the project. Text with line breaks: send the request ended by a NUL
  byte, or put the text in `view-open`'s JSON.

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
{"event":"view_activated","plugin":"my-plugin","id":"results","line":3,"text":"src/main.c:12: warning"}
{"event":"view_closed","plugin":"my-plugin","id":"results"}
```

| Event | When |
|---|---|
| `did_open` | A file has loaded in an editor, including a new file that does not exist yet. Load failures other than "not found" do not send it. |
| `did_change` | A file's text changed, sent once typing pauses (300 ms). It carries only the path: read the text with `get-text`. |
| `did_save` | A file was written, autosaves included. |
| `did_close` | A file's editor tab closed. |
| `active_changed` | Another tab became the active one. The path is empty when the active tab is not an editor. |
| `project_changed` | Another project folder opened. |
| `view_activated` | A line of a plugin's view was double-clicked or got Enter: the plugin (`socket` for a connection without a plugin), the view's id, the line (from 1) and its text. |
| `view_closed` | The user closed a plugin's view tab. |

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
- The Plugins window shows a plugin's permissions, and Browse shows them before a plugin is installed.

### The Plugins window

**Plugins: Manage Plugins** (command palette, the menu bar's Plugins menu, or a click on a
plugin in the Plugins panel) opens a window: every plugin found on the left, with a dot in its
state's color (green running, red failed, hollow off), the one picked on the right.

The **Plugins panel** (its icon in the left dock by default; Panels menu, or "View: Toggle Plugins")
is the short version: each plugin with its dot and its on / off switch, the count at the top with
Reload and a ⋯ menu (Manage Plugins…, Open Plugins Folder, Process Monitor), and **Browse the
Registry…** at the bottom. A click on a plugin opens it in the window.

The menu bar's **Plugins** menu, between Settings and Panels, has Manage Plugins…, Browse the
Registry… ("Plugins: Browse the Registry"), Plugins Panel, Reload Installed Plugins, Open Plugins
Folder and Process Monitor….

- **What it shows:** on top its name, its state (on, off; for a `start` plugin: running, stopped,
  failed, not started), Start / Stop and the switch; under them its version, provider and
  description. Then its commands with their keys, its permissions and what it brings (language
  servers, Run, language specs, snippets). Folded under **Details**: id, provider, version, API
  (`tapi_1`, or none for a plugin from before versions), where it was found (your plugins folder, the
  system, t-code itself), its folder and its registry record. Folded under **Output** (open by itself
  when the plugin failed): the last 200 lines its programs printed, which update while the window is
  open.
- **On / off:** the switch. Off, a plugin's commands and keys leave the palette, its language servers
  and Run stop being used, its start program stops and it does not start again. On, all of that comes
  back (it starts when a project is open). Its snippets and language specs follow at the next start.
  The choice is kept in `~/.config/t-code/plugins.json` (`"disabled"`: the ids of the plugins that are
  off).
- **Start / Stop:** for a `start` plugin, by hand (stopping is the same SIGTERM, then SIGKILL).
- **Open Folder**, and **Uninstall** for a plugin in your plugins folder: it stops, and its folder is
  deleted after a question. Plugins that came with the system or with t-code cannot be uninstalled
  here.
- **Reload** (the arrow at the top left) and **Open Plugins Folder** (the ⋯ menu at the top right):
  put a plugin's folder in `~/.config/t-code/plugins/`, press Reload, and it shows.
- **Process Monitor** in the ⋯ menu opens the Process Monitor ("Help: Process Monitor"): each plugin's
  programs with their CPU and memory, next to t-code's other processes, and End for each.
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
  name, provider, description, version, API, permissions, categories and `"reviewed": false|true`.
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
  entries as above.
- **The signature.** `index.json.sig`, beside the index (the same URL or file plus `.sig`), holds an
  Ed25519 signature over the index's exact bytes, in base64. t-code checks it with the registry's
  public key, which it carries (`$TCODE_REGISTRY_KEY`, a public key in base64, names another for a
  mirror with its own); the registry's `PUBLIC_KEY` file holds the same. A maintainer signs the
  index after each change (the registry's README, "Signing"); the key never goes to GitHub.
- **When it does not check.** An index t-code checked is kept in `~/.cache/t-code/registry/`. When
  the next one cannot be fetched, has no signature or a wrong one (changed on the way, or merged and
  not signed yet), Browse shows the kept one and says why at the bottom ("Showing the registry as of
  ..."). With nothing kept, it shows nothing and says why. Installing still checks each plugin's
  tree hash against the pin of the index it came from.

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

These are plugins the API is meant to make possible. Fifteen of them are written and live in this
repository under `plugins/builtin/` (Java, MATLAB, Octave, ShellCheck, CMake, Text Tools, TODO Finder,
Word Count, Format on Save, Targets, LaTeX and Typst, Pomodoro, Open on GitHub / GitLab, Commit Message Helper, Spell Check), as working examples and
as the registry's first entries. They are written in different languages on purpose, to test that
tapi_1 works the same from any of them: TODO Finder and LaTeX are Rust, Word Count and Spell Check are C++, Pomodoro is C, Format on Save and the Commit Message Helper are Go,
Targets is JavaScript (Node.js), Open on GitHub is Perl, the rest are Python 3.
t-code does not load them by itself (install them from Browse, or copy a folder into `~/.config/t-code/plugins/`). MATLAB and Octave also come as their own packages,
`t-code-matlab` and `t-code-octave` (Fedora, Debian / Ubuntu, Arch, Void: `make install-plugin
PLUGIN=<id>` puts them in `/usr/share/t-code/plugins/<id>/`). Each one says what it does, what t-code provides
(**core**), what the plugin itself does, and where it stops.

### Languages and REPLs

#### Java (`java`, in this repository: `plugins/builtin/java`)
- **Does:** Java with what C gets from clangd, through the Eclipse JDT Language Server (jdtls):
  errors as you type, completion (with imports), hover with Javadoc, signature help, go to
  definition, references, rename, formatting, outline, call and type hierarchy, semantic colors,
  inlay hints and quick fixes. Run (also `Ctrl+F5` on a `.java` file), Debug (`F5`), Build, Run
  Tests, Clean and Stop for Maven, Gradle and plain folders; library classes open as read-only
  sources. Install Language Server, Install Debugger and Clean Language Server Workspace.
- **Core:**
  - An `lsp` entry for `t-code-jdtls` (the plugin's launcher, in `~/.local/bin` once installed;
    until then the Plugins window shows it as not found). jdtls answers hover with MarkedStrings
    and may send a quick fix as the client-side command `java.apply.workspaceEdit`: t-code reads
    both.
  - `"run": {".java": "run"}`, terminal tabs, `diagnostics-set`, `pick`, `status-set`, and
    `lsp-refresh`, so files already open get the server right after the install.
  - `"debug": {".java": "debug"}`, `lsp-command` and `debug-start` for `F5`; `lsp-restart` after
    Install Debugger.
  - `"uri-contents": {"jdt": "java/classFileContents"}` (with `classFileContentsSupport` in its
    options): Go to Definition into a library's class opens its source (or jdtls' decompiled
    text) read-only.
  - The Testing panel lists and runs JUnit tests itself (a Maven or Gradle project); its debug
    button runs a test with its JVM waiting, then hands it to the plugin's `debug` (`TCODE_TEST`,
    `TCODE_DEBUG_PORT`).
  - `"start": "project"`: with jdtls ready the plugin exits at once; without it, the first `.java`
    file that opens asks once whether to install it.
- **Plugin** (Python 3, standard library):
  - **Install:** jdtls needs a JDK 21 or newer (`$JAVA_HOME`, `java` on `PATH`, else the newest in
    `/usr/lib/jvm`). A jdtls already installed (Arch's package, Homebrew, Mason, `jdtls` on `PATH`,
    `$TCODE_JDTLS_HOME`) is used as it is; else the newest milestone is downloaded from
    download.eclipse.org (about 50 MB, its SHA-256 checked) into `~/.local/share/t-code/jdtls/`.
    Then `jdtls_launcher.py` is copied to `~/.local/bin/t-code-jdtls`; a newer plugin version
    refreshes the copy when the project opens.
  - **Launcher:** the JVM with the serial collector and a 1 GB heap at most (on a small project
    295 MB, 800 MB with the parallel collector; `$TCODE_JDTLS_JVM_ARGS` replaces them), jdtls' index
    in `~/.cache/t-code-java/workspace/<project>-<hash>`, Lombok as a Java agent when the build
    file names it. In a project without Maven or Gradle it adds `triggerFiles` (one file per source
    folder) to `initialize`, so `src/com/example/App.java` in package `com.example` is not reported
    as a package mismatch.
  - **Run:** the current file when it has a `main` (JDK 25's `void main()` too), else the only
    one, else the one picked (the last pick first). Maven: `mvn compile
    dependency:build-classpath`, then `java` with that classpath (`mvnw` when the project has one;
    a file under `src/test` gets the test classpath). Gradle: an init script adds a task writing
    the runtime classpath, then `java` (`gradlew` when there is one). Plain folder: `javac
    -sourcepath` into `~/.cache/t-code-java/classes/`, with `lib/*.jar`, then `java` from the same
    JDK as `javac`. Each run is a script typed into the plugin's "Java" tab; stdin and stdout stay
    the terminal's.
  - **Diagnostics:** javac's errors (also through Gradle) with the caret's column, Maven's
    `[ERROR] File.java:[12,5]`, Surefire's and Gradle's failed tests, and the uncaught exception's
    frames in the project (the first an error, each `Caused by` too, the callers info).
  - **Run Tests:** `mvn test` / `gradle test`, only the current class when it is a test.
  - **Debug:** Install Debugger downloads java-debug's newest release from Maven Central (about
    3 MB, its SHA-512 or SHA-1 checked) into `~/.local/share/t-code/java-debug/`; the launcher adds
    it to jdtls' `bundles`, and the plugin restarts the servers. `F5` (or Java: Debug) asks jdtls
    over `lsp-command` for the main class (the current file's, else the only one, else the one
    picked), a build (errors stop it: the Problems panel shows them), the classpath, the Java
    program and a debug adapter's port (`vscode.java.startDebugSession`), then starts t-code's
    debugger on that port with `debug-start`. The program runs in the Debug panel's terminal.
  - **Debug a test** (the Testing panel's debug button): the panel runs the test with its JVM
    waiting on `TCODE_DEBUG_PORT`; the plugin asks jdtls for a debug adapter
    (`vscode.java.startDebugSession`) and attaches it there (`debug-start` with
    `"request": "attach"`).
- **Limits:**
  - Abstract test classes' tests are not listed (their subclasses run them); Gradle's names for
    parameterized cases do not map to their method.
  - A Maven module that needs its sibling modules builds from its own folder.

#### MATLAB (`matlab`, in this repository: `plugins/builtin/matlab`)
- **Does:** write and run `.m` code without the MATLAB desktop: Run File (also `F5` on a `.m` file),
  Run File in Batch, Run Selection, Stop, Open / Restart / Close Session.
- **Core:**
  - Terminal tabs (`terminal-open`, `terminal-send`, `terminal-interrupt`, `terminal-close`,
    `terminal-close-text`), and ending the session with t-code (its figure windows included).
  - `"run": {".m": "run"}`, so `F5` / Run on a `.m` file runs the plugin's command.
  - GtkSourceView's own `matlab` language (no language specs needed); a `.m` file taken for
    Objective-C is left alone.
  - An `lsp` entry for the MATLAB Language Server (`matlab-language-server --stdio`, installed by
    the user; until then the Plugins window shows it as not found).
- **Plugin** (Python 3, standard library; the work is in `mlang.py`, the same file as the Octave
  plugin's, a copy in each folder that `make test` keeps identical):
  - **Session:** a "MATLAB" terminal tab running `matlab -nodesktop -nosplash` (found on `PATH` or
    in `/usr/local/MATLAB/*`, `/opt/MATLAB/*`, `~/MATLAB/*`). Its tab id is kept in
    `$XDG_RUNTIME_DIR/t-code-matlab/`, so every command reaches the same session. Its close text
    is `exit`, so MATLAB saves its history and frees the license when t-code closes.
  - **Run:** `save`, then one line into the session: a `diary` file, the file run inside
    `try` / `catch` with `getReport`, and markers around it. The plugin waits for the end marker, turns
    `Error in foo (line 12)` and `Error: File: … Line: 3 Column: 5` into `diagnostics-set` (the
    first frame an error, the callers info), and clears them on the next run.
  - **Run File in Batch:** the same line given to `matlab -batch` in a "MATLAB Batch" shell tab: no
    session is kept, and its memory goes when the run ends. Its errors become diagnostics the same way.
  - **Run Selection:** `get-selection`, then each line with `terminal-send`.
  - **Stop:** `terminal-interrupt` (Ctrl+C).
  - **Close / Restart:** types `exit` and waits up to 10 seconds for the tab to close by itself,
    then closes it. Restart then opens a new session, which frees the memory MATLAB's graphics
    engine held. Both act only on the plugin's own tab.
- **Limits:**
  - No workspace or variables panel; a variable list could show through `pick`.
  - Plot windows are MATLAB's own.
  - Needs the user's licensed MATLAB.
  - Not visible from inside the Flatpak.

#### Octave (`octave`, in this repository: `plugins/builtin/octave`)
- **Does:** the same commands as MATLAB, for GNU Octave (no license needed): Run File (also `F5`),
  Run File in Batch, Run Selection, Stop, Open / Restart / Close Session.
- **Core:** the same as MATLAB's.
- **Plugin:** MATLAB's `mlang.py` with an Octave flavor:
  - **Session:** an "Octave" tab running `octave --no-gui --quiet` (`$TCODE_OCTAVE` or `octave` on
    `PATH`), close text `exit`.
  - **Run:** Octave has no `getReport` and its `diary` leaves stderr out, so the line prints
    `error: <message>` and each stack frame with its file to stdout (`helper at line 2 column 3
    (/p/lib/helper.m)`). Frames in the project become diagnostics (the first an error, the callers
    info); Octave's own are left out. `syntax error near line 2, column 5 in file …` (older Octave:
    `parse error near line 2 of file …`) marks that place.
  - **Run File in Batch:** `octave --no-gui --quiet --eval '<line>'` in an "Octave Batch" shell tab.
- **Limits:**
  - No language server; diagnostics come only from run errors.
  - With both MATLAB and Octave on, `F5` on a `.m` file goes to the first one found; switch the
    other off in the Plugins window.

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

#### Format on save (`format-on-save`, in this repository: `plugins/builtin/format-on-save`, in Go)
- **Does:** formats the active file when it is saved, where the project configures a formatter:
  clang-format (`.clang-format`), ruff or black (`ruff.toml`, `[tool.ruff]` / `[tool.black]` in
  `pyproject.toml`), prettier (`.prettierrc`, `prettier` in `package.json`), rustfmt (`Cargo.toml`,
  `rustfmt.toml`), shfmt (`.editorconfig`); gofmt and `zig fmt` always. **Format: Format This File**
  (`Ctrl+Alt+F`) formats now, configured or not; **Format: Turn Format on Save Off or On for This
  Project** keeps the choice in `~/.config/t-code-format/settings.json`.
- **Plugin:** starts with the project. On `did_save` (not `"by":"plugin"`, so its own save does not
  loop) it runs the formatter on the buffer's text and applies the difference as one `edit` (the
  common start and end left alone), so the cursor stays and undo is one step, then saves. `run.sh`
  builds it with `go`, offline, into `~/.cache/t-code-format-on-save/`.
- **Limits:** the active file only (requests act on the active editor); a file typed in while the
  formatter ran is left for the next save; the formatter itself is not installed by the plugin.

#### Spell check (`spell`, in this repository: `plugins/builtin/spell`, in C++)
- **Does:** when a file opens or is saved, checks all of a Markdown, text or reST file (fenced and
  inline code, link targets, tags and URLs left out) and the comments of a source file (`//` and
  `/* */`, or `#`) with `hunspell -a`; a word it does not know becomes an `info` diagnostic with up
  to five suggestions. camelCase, ALLCAPS, words with digits or underscores, paths and words under
  three letters are left alone. **Spell: Fix the Word at the Cursor** (`Ctrl+Alt+S`) offers the
  suggestions with `pick` and writes the chosen one with `edit`; **Add the Word at the Cursor to My
  Dictionary** keeps it in `~/.config/t-code-spell/words`; **Check This File** checks the unsaved
  text now.
- **Plugin:** C++17 and POSIX; starts with the project. Dictionaries: `dictionaries=en_US,tr_TR` in
  `~/.config/t-code-spell/settings`, else the language of `$LANG` with `en_US` (a word is right
  when either knows it). `run.sh` builds it with the system's C++ compiler into
  `~/.cache/t-code-spell/`.
- **Limits:** strings in code are not checked; suggestions cannot appear in the right-click menu;
  the underlines follow the last check (open, save, Check This File), not each keystroke.

### Documents

#### LaTeX and Typst (`latex`, in this repository: `plugins/builtin/latex`, in Rust)
- **Does:** builds a document when one of its files is saved: LaTeX with `latexmk -pdf` (else
  `tectonic`, else `pdflatex`, all with `-file-line-error` or the like), Typst with `typst compile
  --diagnostic-format short`. Its errors and warnings go to the Problems panel; a status bar item
  says `LaTeX: building…`, `LaTeX: ok`, `LaTeX: 2 errors`, or what to install. **LaTeX: Build This
  Document** builds now; **LaTeX: Open the PDF** opens it with `xdg-open`.
- **Plugin:** starts with the project. The document is the file a chapter names (`% !TEX root =
  main.tex`), the file itself when it has a `\documentclass`, else the `main.tex` (or the only file
  with a `\documentclass`) of its folder or the project's; for Typst, a `main.typ` there, else the
  file. `run.sh` builds it with cargo, offline, into `~/.cache/t-code-latex/`.
- **Limits:** the PDF cannot show inside t-code; no SyncTeX jumps yet (inverse search from zathura
  works without the plugin: `--synctex-editor-command 't-code --send "open %{input}:%{line}"'`); no
  language server entry (texlab, tinymist): add one in a plugin of your own; a build ends after five
  minutes.

#### Diagrams (`diagrams`)
- **Does:** renders a Mermaid, PlantUML or Graphviz file or selection to SVG (`mmdc`, `plantuml`,
  `dot`) and opens it with `xdg-open`.
- **Limits:** cannot draw inside t-code's Markdown preview; the preview is part of t-code.

#### Word count (`word-count`, in this repository: `plugins/builtin/word-count`)
- **Does:** the active tab's words and lines as a lasting item at the right of the status bar
  (`status-set`), unsaved edits included (`get-text`), updated on `did_change` and
  `active_changed`; no item for a tab that is not an editor. **Word Count: Count Words in the
  Selection or Tab** (`Ctrl+Alt+W`) says the selection's count, or the tab's without one.
- **Plugin:** C++17, the standard library and POSIX sockets; starts with the project. `run.sh`
  builds it with the system's C++ compiler into `~/.cache/t-code-word-count/`, once per version of
  its source.
- **Limits:** the item follows the typing (after its 300 ms pause), not the selection: t-code sends
  no event when only the selection changes.

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

#### Make / just / Task targets (`targets`, in this repository: `plugins/builtin/targets`, in JavaScript)
- **Does:** **Targets: Run a Target** (`Ctrl+Alt+M`) lists the project's Makefile, justfile,
  Taskfile and `package.json` scripts with `pick` (a target's `## comment` or the comment above it,
  a task's `desc:`, a script's command as the detail) and runs the chosen one in a "Targets"
  terminal tab; **Targets: Run the Last Target Again** (`Ctrl+Alt+R`). When it ends, compiler
  messages in its output (`file:line:col: error|warning: ...`, following make's "Entering
  directory") become diagnostics and the status bar says done or failed.
- **Plugin:** Node.js 18 or newer, standard library. The tab runs a script in
  `$XDG_RUNTIME_DIR/t-code-targets/` that keeps the output and the exit status; the plugin closes
  its connection while the target runs and comes back for the results.
- **Limits:** the project's top folder only; Make targets come from reading the Makefile (no
  `include`d files, no generated rules); a target started while another still runs in the tab is
  typed into it.

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
- **Does:** a plugin starts its adapter with `debug-start` (its command line, or a port), from the
  command its `debug` names for `F5`. debugpy, Delve and lldb-dap are built in; codelldb would come
  this way.
- **Core:** done for tapi_1 (`debug-start`, `"debug"`); the Java plugin is the first user.

### Git and forges

#### Open on GitHub / GitLab (`forge-link`, in this repository: `plugins/builtin/forge-link`, in Perl)
- **Does:** **Open on GitHub: Copy Link to This Line** puts the web link of the cursor's line, or
  of the selected lines, on the clipboard (`copy`); **Open This Line in the Browser** opens it with
  `xdg-open`. The link names the current commit (`…/blob/<commit>/src/main.c#L10-L12`), so it
  keeps showing the same text; the status bar warns when that commit is not pushed yet.
- **Plugin:** Perl 5, core modules. The remote is the one the branch tracks, else `origin`; the
  link has GitHub's form, GitLab's (hosts with "gitlab" in their name), Codeberg / Gitea /
  Forgejo's or Bitbucket's.
- **Limits:** other self-hosted forges get GitHub's form; a file not in git yet has no link. Fedora
  splits Perl's core modules into packages: the plugin needs `perl-IO`, `perl-PathTools`,
  `perl-File-Basename` and `perl-Socket` (or all of `perl`).

#### GitHub / GitLab (`forge`)
- **Does:** uses the `gh` or `glab` command-line tools.
  - Open on GitHub: the line's link, opened with `xdg-open`.
  - Copy Permalink: through `copy`.
  - Check out PR: the open PRs in `pick`, then switch to the chosen PR's branch.
  - Create PR: runs `gh pr create` in a terminal tab.
- **Limits:** no review UI or comment panel.

#### Commit message helper (`commit-msg`, in this repository: `plugins/builtin/commit-msg`, in Go)
- **Does:** **Commit: Write the Message for the Staged Files** looks at the staged files and offers
  the Conventional Commits type they look like first in `pick` (only docs: `docs`; only tests:
  `test`; CI files: `ci`; build and packaging files: `build`; a new source file: `feat`; else
  `fix`), with the scope they share (`src/lsp/…` → `lsp`, `plugins/builtin/java/…` → `java`,
  preferring scopes the history used), then asks for the subject (`ask`) and fills Source Control's
  message box with `scm-message`: `feat(lsp): hover for Java`. A repository whose last commits are
  mostly not Conventional Commits gets the subject alone; closing the question leaves the prefix in
  the box to type after.
- **Plugin:** Go, standard library (the same `tcode.go` as Format on Save); `run.sh` builds it with
  `go` into `~/.cache/t-code-commit-msg/`.
- **Limits:** a guess from paths, not from what the change does; the user still commits.

### Everyday tools

#### TODO finder (`todos`, in this repository: `plugins/builtin/todos`, in Rust)
- **Does:** finds TODO, FIXME, XXX and HACK comments (git's file list in a repository, so ignored
  files stay out; else a walk without hidden folders and build output). **TODO: Go to a TODO in the
  Project** lists them in the picker (text, then file and line) and opens the chosen one; **TODO: List
  TODOs in the Problems Panel** puts them there as `info` diagnostics; **TODO: Clear ...** removes them.
- **Plugin:** Rust source; `run.sh` builds it once with cargo (offline, no dependencies) into
  `~/.cache/t-code-todos/`, again when the source changes.
- **Limits:** needs cargo for the first run; the Problems panel's list is as of the command (it does
  not follow edits); at most 5,000 TODOs, files up to 1 MB.

#### Text tools (`text-tools`, in this repository: `plugins/builtin/text-tools`)
- **Does:** UPPER / lower / Title / snake_case / camelCase, sort (both ways), remove duplicate
  lines, reverse lines, trim trailing spaces, base64 and URL encode / decode, format / minify JSON,
  insert a UUID or the date and time.
- **Plugin** (Python 3): `get-selection-range` and `get-selection`, then one `edit` (one undo step).
  The line and JSON commands take the whole buffer when nothing is selected; the others need a
  selection.

#### Snippet packs and themes
Declarations only: `snippets/` and `styles/*.xml` folders, no program (see [Color themes](#color-themes)).

#### Time tracking (`wakatime`)
- **Does:** passes `active_changed` and `did_save` to `wakatime-cli`.
- **Permissions:** declares `network`.

#### Pomodoro (`pomodoro`, in this repository: `plugins/builtin/pomodoro`, in C)
- **Does:** **Pomodoro: Start a Focus Session** puts a countdown at the right of the status bar
  (`🍅 24:59`, `status-set`); then a break starts by itself (`☕ 4:59`, a long one after every fourth
  session), and the session after it waits for you. Each end is said in the status bar and, with
  `notify-send` installed, as a desktop notification. **Pause or Resume** (`⏸ 🍅 12:00`) and
  **Stop**. Durations in `~/.config/t-code-pomodoro/settings`: `focus=50`, `break=10`, `long=20`
  (minutes; 25, 5 and 15 without it).
- **Plugin:** C11 and POSIX. The process of **Start** stays and counts down; the other commands
  change a small state file in `$XDG_RUNTIME_DIR/t-code-pomodoro/` that it reads four times a
  second. `run.sh` builds it with the system's C compiler into `~/.cache/t-code-pomodoro/`.
- **Limits:** a countdown lives as long as t-code (closing it ends the countdown; the state file
  stays, Start begins anew).

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
