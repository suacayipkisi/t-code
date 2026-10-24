# Plugin System Design for t-code

This document describes `t-code`'s plugin architecture: Tier 1 (process plugins) is implemented
since 0.3.1; Tier 2 (native modules) is an idea, not implemented.

## Philosophy

`t-code`'s core strength is its fast startup, minimal memory footprint (the window itself about 70 MB PSS with 25 open tabs, measured for 0.3), and responsiveness. Any plugin mechanism must uphold these invariants:

1. **Zero idle overhead:** Installing plugins must not degrade performance or increase idle RAM if those plugins are not active.
2. **Crash isolation:** A malfunctioning plugin must never crash the editor window or freeze the GTK main loop.
3. **No embedded heavy runtimes:** Avoid embedding a heavy JavaScript engine (V8, QuickJS) or Python runtime inside the core binary.
4. **Simple, language-agnostic integration:** Anyone can author a plugin using shell scripts, C, Rust, Go, Python, or any tool that reads and writes standard streams or sockets.

---

## Architecture: Two-Tier Plugin Model

### Tier 1: IPC Process Plugins (Recommended & Primary)

Plugins run as independent subprocesses communicating with `t-code` via its UNIX domain socket (`$TCODE_SOCKET`, documented in `src/ipc/socket.h`). A plugin runs with the user's own rights and can read, change and save the open files: install only plugins you trust.

#### Manifest (`plugin.json`)

Plugins are discovered in `~/.config/t-code/plugins/<plugin_id>/` (created if missing),
`/usr/share/t-code/plugins/<plugin_id>/` (this exact path, whatever the install prefix), and
`plugins/<plugin_id>/` next to t-code's `styles/` folder: in the source tree that is the repository's
`plugins/`, with the `word-count` example. `make install` and the packages install no plugins.
When two folders hold the same plugin id, the first one found wins (user folder first).

`name` and a non-empty `commands` list are required; each command needs `id` and `name`. `id`
(optional) defaults to the plugin's folder name. `executable` is relative to the plugin's folder
(or absolute); without it, t-code looks for an executable `run.sh`, `plugin`, or a file named like
the folder. A plugin without a runnable executable or without valid commands is skipped with a
warning on stderr.

```json
{
  "name": "Git Blame",
  "version": "1.0.0",
  "description": "Shows blame for the active line",
  "executable": "blame.sh",
  "commands": [
    {
      "id": "git-blame.show",
      "name": "Git: Show Line Blame",
      "keys": "Ctrl+Alt+B",
      "trigger": "<Control><Alt>b"
    }
  ]
}
```

`keys` is the label shown beside the command. `trigger` is GTK's shortcut syntax; commands without
it remain available in the palette. When duplicate triggers are found, the first discovered plugin
command keeps the binding; invalid triggers are skipped. Reload Installed Plugins refreshes bindings
in both windows.

#### Socket Protocol

The control socket (`$TCODE_SOCKET`) takes one request per line, `<command> <argument>`. Paths
must be absolute. "The active tab" is the current tab of the focused editor group; the buffer
commands do nothing when it is not an editor.

| Command | Arguments | Description |
|---|---|---|
| `open` | `<path>` | Opens a file in the active editor group |
| `open-side` | `<path>` | Opens a file in the right editor group |
| `open-window` | `<path>` | Opens a file in the second window |
| `preview` | `<path>` | Opens a Markdown preview tab |
| `compare` | `<path>` | Compares the file with its last commit (side by side) |
| `history` | `<path>` | Picks a commit from the file's history to compare with |
| `project` | `<dir>` | Switches to another project folder |
| `terminal` | `<dir>` | `cd` to the folder in the shell |
| `get-file` | *(none)* | Returns the active tab's file path (one line; empty if none) |
| `get-project` | *(none)* | Returns the project's root folder (one line) |
| `get-text` | *(none)* | Returns the active editor buffer, terminated by NUL so multiline text is unambiguous |
| `insert` | `<text>` | Inserts text at the cursor position |
| `append` | `<text>` | Appends text at the end of the buffer |
| `replace-all`| `<text>` | Replaces the buffer content |
| `save` | *(none)* | Saves the active editor tab |
| `say` | `<message>` | Shows a short message in the status bar (the next status update replaces it) |
| `copy` | `<text>` | Puts text on the system clipboard |
| `debug` | `<program>` | Starts the debugger on a program that was just built (used by F5's build) |
| `subscribe` | *(none)* | Keeps the connection open and streams JSON-lines document events |

`debug-failed` also exists, for F5's build to say that it failed. `open` takes no line or column
yet.

Requests normally end with a newline. Send NUL-terminated requests when an argument contains
newlines, such as `replace-all`; a connection using NUL framing should terminate each request with
NUL. `get-file` and `get-project` return one newline-terminated response. `get-text` returns the
full buffer followed by a NUL byte, preserving embedded newlines and distinguishing an empty buffer
from a closed connection.

Send `subscribe\n` and keep the socket connection open. The server first replies
`{"event":"subscribed"}\n`, then sends one JSON object per line:

```json
{"event":"did_open","path":"/absolute/path/to/file.c"}
{"event":"did_save","path":"/absolute/path/to/file.c"}
```

`did_open` is sent once the file has loaded, including a new file that does not exist yet; load
failures other than “not found” do not emit it. `did_save` follows a successful disk write, including
autosaves. Closing the client connection unsubscribes it.

#### Execution Flow

1. User invokes a plugin command via the Command Palette (`Ctrl+Shift+P` or `F1`) or its assigned
   shortcut.
2. `t-code` sets `$TCODE_SOCKET`, `$TCODE_ACTIVE_FILE`, `$TCODE_PROJECT_ROOT`, `$TCODE_PLUGIN_ID`,
   and `$TCODE_COMMAND_ID`, then starts the executable without waiting for it, in the plugin's
   folder, with two arguments: the command id and the active file (empty when the active tab is
   not an editor). Its output goes to t-code's own stdout / stderr; a non-zero exit shows
   "Plugin <name> failed" in the status bar.
3. The plugin can make one-shot socket requests and exit, or keep a `subscribe` connection open to
   receive `did_open` and `did_save` events.

---

### Tier 2: Native GModule C Extensions (idea, not implemented)

For extensions requiring deep GTK4 widget hierarchy integration (such as custom panels, custom diff gutters, or custom syntax highlighters), a C GModule (`.so`) interface will be exposed:

```c
typedef struct {
  const char *name;
  const char *version;
  void (*init)(App *t);
  void (*shutdown)(App *t);
} TCodePlugin;
```

Native plugins are loaded via `g_module_open()` on demand. They are intended for performance-critical extensions where subprocess IPC latency is unacceptable.

---

## Status

All of the following shipped in t-code 0.3.1:

- Socket commands for plugins (`get-file`, `get-project`, `get-text`, `insert`, `append`,
  `replace-all`, `save`, `say`) and `$TCODE_SOCKET` in every plugin's environment.
- Plugin manifest discovery, palette commands and keyboard shortcut binding ("Plugins: Reload
  Installed Plugins", "Plugins: Open Plugins Folder").
- Event notifications (`did_open`, `did_save`) over a `subscribe` connection.

Not planned for a version yet: Tier 2 native modules, `open` with a line and column, more events.
