# <img src="desktop/io.github.tcode.svg" alt="" width="48" align="top"> T-Code

A light C/C++ IDE for Linux in one GTK4 window, with a dark look (Dark Classic) and other color themes.

### Jump => [Latest Release v0.3.2](https://github.com/suacayipkisi/t-code/releases/tag/v0.3.2) | [Install](https://github.com/suacayipkisi/t-code#install) | [Usage and Keys](https://github.com/suacayipkisi/t-code#usage) | [Performance](https://github.com/suacayipkisi/t-code#performance)

![t-code 0.3 in the Dark Classic color theme with the rounded look](docs/dark_classic-rounded.png)

![t-code 0.3 in the Gruvbox color theme with the square look](docs/gruvbox-square.png)

<sub>t-code 0.3: Dark Classic, rounded (top) and Gruvbox, square (bottom).</sub>

**New in 0.3.2:** fixes for the update release URL launcher and clangd compile command flag escaping.

**In 0.3.1:** language servers for **Python, Go and Rust** (`pylsp`, `gopls`, `rust-analyzer`, when installed) next to clangd, an out-of-process **Plugin System** (over the UNIX socket `$TCODE_SOCKET`), a **Problems panel** (`Ctrl+Shift+M`), **project tasks** (`.t-code/tasks.json`) and a **Test** command, Meson projects, **crash recovery** for unsaved changes, staging single hunks, Go to Symbol in Workspace, a check for new releases, and packages for **Flatpak**, **Snap**, **AUR** and **AppImage** alongside Fedora, Arch, Debian and Void Linux.

**Next (0.4):** multiple terminals, as tabs in the shell panel (planned for 0.3.1, postponed).

**In 0.3:** a debugger (gdb: breakpoints, stepping, variables, call stack, watch, conditional
breakpoints, values under the pointer, disassembly), quick fixes, code folding, sticky scroll, color
themes, panels that move between docks, a second window, right-click menus everywhere, and Run for
Python, JavaScript, Rust, Go, Java and other languages' projects.

`t-code --version` prints the installed version. The editor tabs are
[GtkSourceView 5](https://gitlab.gnome.org/GNOME/gtksourceview) views (since 0.2); the older t-code
whose tabs ran [micro](https://micro-editor.github.io) in terminals (0.1.1) lives on in the
[`micro-main`](../../tree/micro-main) branch.

One window, one process (besides its language servers, your shell, and plugins):

- **left:** the Files panel (open tabs over the project's folder tree, with file icons and git
  colors), Source Control, Search, and Run and Debug
- **middle:** editor tabs (GtkSourceView), sharing one language server per language
- **bottom:** your shell (where Build, Run, Test and tasks run), the debugged program's terminal,
  and the **Problems panel** (`Ctrl+Shift+M`)

Every panel can be dragged by its icon to another dock (left, bottom, right, stacked or side by
side), hidden and shown again from its button in the title bar; "View: Reset Panel Layout" puts
them all back.

### The editor

Like a plain text editor, without modes: `Ctrl+C` / `Ctrl+V`, `Ctrl+Z`, `Shift`+arrows to select,
the mouse. On top of that:

- **clangd's problems** are underlined with a wave (red errors, yellow warnings); resting the
  pointer on them shows the message, and on a symbol its type and signature. The status bar counts
  them, and a strip right of the minimap marks where they are in the whole file (click to go there).
- **Completion while typing** (and after `.`, `->`, `::`, or on `Ctrl+Space`): the list shows each
  symbol's kind, its parameters and type; a function goes in with its parameters as placeholders
  (`Tab` to the next one), and the `#include` it needs is added by itself.
- **Parameter hints**: in a call's arguments, the signature shows above the cursor with the
  argument being typed in bold.
- **Go to Definition** (`F12`, `Ctrl+Click`) and back (`Alt+←`), **Find All References**
  (`Shift+F12`), **Rename Symbol** (`F2`, in every file), **Format Document** (`Shift+Alt+F`, with
  the project's `.clang-format`), **Go to Symbol in File** (`Ctrl+Shift+.`), **Go to Symbol in
  Workspace** (from the command palette), **Switch Source / Header** (`Alt+O`).
- **Quick fixes** (`Ctrl+.`): clangd's fixes for the problems on the line (a missing `;` or
  `#include`, a misspelled name) and its refactorings, one pick away.
- **Code folding** (the arrow beside a block, `Ctrl+Shift+[` / `]`) and **sticky scroll**: the
  heads of the function, loop or namespace you are in stay at the top of the editor.
- **Colors by meaning**: clangd tells functions (yellow), types (teal), macros (blue), constants and
  enum members (bright blue) and variables (light blue) apart; GtkSourceView colors keywords,
  strings, comments and the rest from its theme (`styles/dark-classic.xml`). `.h` files
  open as C++ when they contain C++.
- Find and replace (`Ctrl+F`, `Ctrl+H`; case, whole word, regular expressions), go to line
  (`Ctrl+G`), comment lines (`Ctrl+/`), duplicate (`Ctrl+D`), move (`Alt+↑` / `Alt+↓`) and delete
  (`Ctrl+Shift+K`) lines, brackets and quotes that close by themselves (and wrap a selection).
- `Enter` after an opening bracket goes one level deeper (between a pair the closer gets its own
  line), and `}` outdents. A double click selects a name only (letters, digits, `_`).
- **Highlights**: the cursor on a name lights up that symbol's uses (clangd's, not other names
  spelled the same); selecting a name lights up its text everywhere. Their lines are marked in the
  strip beside the minimap, as are git's changes. Code an `#if` leaves out is dimmed.
- **Right-click menus** on the text, tabs, files, Source Control's changes and commits, search
  results, terminals and the debugger's lists.
- A minimap, indent guides (the block around the cursor in its bracket color), the git diff gutter
  (green added, blue changed, red deleted), autosave a second after the last change.

clangd starts when the first C or C++ file is shown; Python (`.py`), Go (`.go`) and Rust (`.rs`)
files use `pylsp`, `gopls` and `rust-analyzer` respectively when the executable is on `PATH`.
Each server starts on first use and stops a minute after the last file it serves closes. Files are
sent only once their tab is shown, so restored sessions cost the servers little. Other files
(Markdown, Makefiles and scripts) get completion from the words in the file. "Restart Language
Servers" in the command palette restarts active servers (including one that gave up after crashes);
after installing an optional server, reopen its file to activate it. When clangd cannot find
includes and the project has no `compile_commands.json` or `compile_flags.txt`, the status bar shows
**⚠ No compile flags**; a click explains how to give it the flags (and points out a misnamed
`compile-flags.txt`).

## Requirements

`clangd`, `git`, the Cascadia Code font (`ripgrep` is optional, for a faster Search panel; `gdb` 14
or newer built with Python, for debugging), and to
build: `gcc` 14 (C23), `make` and the GTK4, GtkSourceView 5 (5.16 or newer), json-glib, VTE and
md4c headers. The packages below pull in all of these by themselves.

```sh
sudo dnf install gtk4-devel gtksourceview5-devel json-glib-devel vte291-gtk4-devel md4c-devel
```

## Install

### With a package (Fedora, Arch Linux, Debian, Void Linux, AUR)

#### [Latest Release v0.3.2](https://github.com/suacayipkisi/t-code/releases/tag/v0.3.2)

t-code 0.3.2 has packages for Fedora 44, Arch Linux, Debian 13 (trixie) and Void Linux (glibc and
musl), x86_64. Install the one for your distribution; the package manager pulls in what it needs
(clangd, git and the Cascadia Code font among them):

```sh
sudo dnf install ./t-code-0.3.2-1.fc44.x86_64.rpm        # Fedora
sudo pacman -U t-code-0.3.2-1-x86_64.pkg.tar.zst         # Arch Linux
sudo apt install ./t-code_0.3.2-1_amd64.deb              # Debian
```

On Void Linux, xbps installs from a repository, so the downloaded package is indexed first (in
the folder it is in; use the `x86_64-musl` file on musl):

```sh
xbps-rindex -a t-code-0.3.2_1.x86_64.xbps
sudo xbps-install --repository="$PWD" t-code
```

Void has no package for Cascadia Code: t-code then uses the system's monospace font, and any
font can be picked in Settings. The Void packages were built and tested in Void containers.

Then start `t-code` from the app menu, or `t-code [folder]` in a terminal. To remove it, use
your package manager (`sudo dnf remove t-code`, `sudo pacman -R t-code`, `sudo apt remove t-code`,
`sudo xbps-remove t-code`).

A package puts `t-code` in `/usr/bin`, the editor's colors in `/usr/share/t-code/styles/`, and the
app menu entry and icon.

### AUR source package (not published to AUR yet)

The repository includes a pinned, checksum-verified `packaging/aur/PKGBUILD` for t-code 0.3.2.
After checking out this repository, build and install it with Arch's `base-devel` tools:

```sh
cd packaging/aur
makepkg -si
```

This is a source recipe, not a published AUR listing. Release artifacts for Flatpak and Snap are attached below.

### Flatpak (x86_64)

Install the unsigned Flatpak bundle from a tag built with this packaging workflow:

```sh
flatpak install --user ./t-code-0.3.2-x86_64.flatpak
flatpak run --file-forwarding io.github.tcode @@ "$HOME/projects/example" @@
```

Open a whole project folder through the folder chooser or file forwarding so the document portal
grants access to its contents. The sandbox does not inherit host credentials, tools, or plugin
configuration. See [`packaging/flatpak/README.md`](packaging/flatpak/README.md) for permissions,
toolchain, and access details.

### Snap (Linux amd64)

The release Snap is strictly confined and not yet published to the Snap Store. Install the
unsigned release artifact locally:

```sh
sudo snap install --dangerous ./t-code_0.3.2_amd64.snap
snap run t-code "$HOME/projects/example"
```

The Snap can access projects under non-hidden directories in your home. Its configuration and
plugins are private to the Snap; host credentials, executables and development libraries are not
imported. F5 debugger tracing is unavailable under strict confinement. See
[`packaging/snap/README.md`](packaging/snap/README.md) for the exact boundaries.

### As an AppImage (x86_64, for everything else)

[t-code-x86_64.AppImage](https://github.com/suacayipkisi/t-code/releases): a single file without an
install — t-code with its GTK4, GtkSourceView, VTE and the rest bundled inside. From the
distribution it takes `clangd` and `git`, and what every desktop already has and must match its
graphics drivers and font settings: the Wayland and X11 client libraries, fontconfig, FreeType and
HarfBuzz. Built by `packaging/appimage.sh` like the packages above.

### Building the packages yourself

The distribution packages are built in containers using `podman` or `docker`. The AUR recipe
builds in an Arch container. Flatpak needs Linux, `flatpak`, `flatpak-builder`, `jq`, and access to
Flathub. Snap needs Linux amd64, Snapcraft 9, and initialized LXD; its fully confined smoke test
requires a native snapd/AppArmor host, not a container.

The shared scripts require Bash 4 or newer; macOS's bundled Bash 3.2 is too old. Flatpak and Snap
builds require Linux even when native distribution packages can use containers.

```sh
packaging/build.sh                 # all formats; requires the tools above
packaging/build.sh aur             # AUR source package
packaging/build.sh flatpak         # Flatpak bundle and repository
packaging/build.sh snap            # strict Snap package (Linux amd64 + LXD)
packaging/test.sh                  # all package smoke tests on a prepared Linux host
packaging/test.sh aur flatpak      # choose package tests
```

Artifacts land in `packaging/packages/<format>/`; the release directory contains installable RPM,
Arch, DEB, XBPS, AppImage, Flatpak and Snap files, Snapcraft build/dependency provenance, and a
combined `SHA256SUMS`. The AUR build artifact is validated but is not attached as a release package;
publish the recipe separately to AUR after account/repository setup.
On Fedora, the RPM is built directly with `rpmbuild`; install its build dependencies once with
`sudo dnf install rpm-build dnf-plugins-core && sudo dnf builddep packaging/fedora/t-code.spec`.

`packaging/test.sh` installs each distribution package into a fresh container, resolves its
dependencies, checks installed files and starts t-code under Xvfb with clangd, a project, preview,
and socket interactions. Flatpak smoke installs the bundle in a temporary user installation and
launches it in its real sandbox. Snap smoke requires a disposable Linux host with strict
confinement and passwordless sudo; it installs the unsigned package and leaves it installed for
inspection. The package build also runs tests that do not need a display.

### From the source, into ~/.local

```sh
packaging/install.sh
```

This builds t-code and installs it under `~/.local`: the `t-code` command, the editor's colors in
`share/t-code/styles/`, and a "t-code" entry in the app menu
with its own icon (`desktop/io.github.tcode.svg`, installed to `~/.local/share/icons`). Run it again after changing the code to update the installed copy.

To remove everything it installed:

```sh
packaging/uninstall.sh
```

`make install` installs system-wide instead (`PREFIX=/usr/local` by default, `DESTDIR` for
staging; the packages use it). For development, `make` builds into `build/`, and `build/t-code`
runs straight from the repo.
`make test` runs the tests in `tests/` (the editor, language server, Search panel and Source Control
ones need a display; they open no window). `make test-asan` runs the ones without GTK again under
AddressSanitizer, which also fails on memory leaks. `.clang-format` is the code's style
(2 spaces); clangd and Format Document use it. `tools/` has scripts that measure what a
running t-code costs in memory and CPU (`tools/proc-report.sh`), or run it invisibly to measure a
change (`tools/headless.sh`).

## Plugins

t-code runs plugins as separate programs, started only when one of their commands is used, so an
installed plugin costs nothing while idle. Plugins are discovered in
`~/.config/t-code/plugins/<plugin_id>/` and `/usr/share/t-code/plugins/<plugin_id>/` (and, running
from the source tree, `plugins/`, which holds the `word-count` example). A plugin runs with your
user's rights: install only plugins you trust.

Each plugin provides a `plugin.json` manifest:

```json
{
  "name": "Word Count",
  "version": "1.0.0",
  "description": "Counts words in active editor",
  "executable": "wordcount.sh",
  "commands": [
    {
      "id": "count",
      "name": "Word Count: Count Words in Current Tab",
      "keys": "Ctrl+Alt+W",
      "trigger": "<Control><Alt>w"
    }
  ]
}
```

When invoked from the Command Palette (`Ctrl+Shift+P`) or its manifest shortcut:
1. The executable is started in its plugin's folder, with the command id and the active file as
   its arguments and these environment variables:
   - `TCODE_SOCKET`: UNIX control socket path.
   - `TCODE_PLUGIN_ID`: plugin identifier.
   - `TCODE_COMMAND_ID`: executed command identifier.
   - `TCODE_ACTIVE_FILE`: absolute path to active editor file.
   - `TCODE_PROJECT_ROOT`: root directory of the current project.
2. The plugin interacts with t-code via `$TCODE_SOCKET` (for example, `say <message>`, `get-file`,
   `get-project`, `get-text`, `replace-all <text>`, `insert <text>`, `append <text>`, and `save`).
   Sending `subscribe` keeps the socket open for JSON-lines `did_open` and `did_save` events.
3. The manifest's `keys` value labels the palette command; optional GTK `trigger` binds a shortcut.
   The command remains available in the palette either way. One-shot plugins can exit immediately;
   an event subscriber stays connected until it closes its socket.
See [`docs/plugins.md`](docs/plugins.md) for the manifest and socket protocol.

## Creating a Plugin

If you are interested in writing a plugin for t-code, all types of plugins will be gladly accepted. However, you also have to consider the main idea of t-code's existence, which is as low resource usage as possible. Because of that I highly recommend writing the plugin in the C language, which is the language t-code is written in. You can also use AI. Also, the plugin must be safe.

## Contributing

If you want to contribute, all contributions will be gladly accepted. Please read
[`CONTRIBUTING.md`](CONTRIBUTING.md) and open a pull request.

## AI Usage in Project  

Most of the structure and code written with using AI coding agent. Every single line of code is reviewed or currently under review. I did not spend enough time to write just myself because I just want an ide that uses not much ram as immediately as possible, the editor I used was using too much then I got angry and tada... BUT I will review every detail of this project and after than I will continue to add new features as I wish.

## Usage

```sh
t-code            # current directory
t-code path/dir
t-code a.c b.c    # open files as tabs
```

### Keys

The **⌨ Keybinds** button in the status bar shows all of them, and `Ctrl+Shift+P` (command
palette) finds any command by name.

| Where       | Key                                    | Action                                    |
|-------------|----------------------------------------|-------------------------------------------|
| anywhere    | `Ctrl+P`                               | open a file by typing part of its name    |
| anywhere    | `Ctrl+Shift+F`                         | search the text of all files (the Search tab) |
| anywhere    | `Ctrl+Shift+P`, `F1`                   | command palette                           |
| anywhere    | `Ctrl+G`                               | go to line                                |
| anywhere    | `F5` / `Ctrl+F5` / `Ctrl+Shift+B`      | debug (continue when paused) / build and run / build only (see below) |
| anywhere    | command palette: `Test`              | run the detected or configured project tests in the shell |
| anywhere    | `F9`, `F10` / `F11` / `Shift+F11`      | breakpoint / step over / into / out (debugging) |
| anywhere    | `Shift+F5` / `Ctrl+Shift+F5` / `F6`    | stop / restart / pause (debugging)        |
| anywhere    | `Ctrl+Shift+D` / `Ctrl+Shift+G`        | Run and Debug / Source Control panel      |
| anywhere    | `Ctrl+Shift+M`                         | Problems panel (errors and warnings across files) |
| anywhere    | `Ctrl+Alt+D` / `Ctrl+Alt+H`            | compare with the last commit / with a commit from the file's history |
| anywhere    | `Ctrl+Shift+V`                         | markdown preview of the current file      |
| anywhere    | `Ctrl+Shift+O`                         | open another project folder               |
| anywhere    | `Ctrl+\`                               | split the editor into two groups / join   |
| anywhere    | `Ctrl+1` / `Ctrl+2`                    | left / right editor group                 |
| anywhere    | `Ctrl+PageDown` / `Ctrl+PageUp`, `Alt+.` / `Alt+,` | next / previous tab           |
| anywhere    | `Alt+1` / `Alt+2` / `Alt+3`            | Files / editor / shell                    |
| anywhere    | `Ctrl+B` / `Ctrl+J`                    | hide / show the Files panel / the shell   |
| anywhere    | `F7`                                   | code coloring on / off (all tabs)         |
| anywhere    | `Ctrl+=` / `Ctrl+-` / `Ctrl+0`         | bigger / smaller / normal text            |
| shell       | `Ctrl+Click` on `file.c:12:5`          | open the file at that line (compiler messages) |
| tab         | left click / middle click / `×`        | switch to / close / close                 |
| tab         | double click                           | keep a temporary (italic) tab             |
| files       | click, `Enter`, `l`                    | open or close a folder, or open the file in a temporary tab (italic; the next click replaces it) |
| files       | double click / middle click            | open the file for good / to the side      |
| files       | `→` / `←`, `h`, `Backspace`            | open a folder or go into it / close it or go to its parent |
| files       | `↑` `↓`, `j` `k`, `g` `G`, wheel        | move / first / last / scroll              |
| files       | `a` / `r`, `F2` / `d`, `Delete`        | new file (`a/b.c` makes folders, end with `/` for a folder) / rename / delete |
| files       | `Ctrl+X` / `Ctrl+C` / `Ctrl+V`         | cut / copy / paste files and folders      |
| files       | drag a row onto a folder / the editors | move it there / open it                   |
| files       | `.` / `R`                              | hide / show dotfiles (shown by default) / read the tree again |
| files       | right click                            | Open, Open to the Side, Open Preview, Compare with HEAD, File History, New File/Folder, Open in Terminal, Cut/Copy/Paste, Copy (Relative) Path, Rename, Delete, Open Folder as Project |
| editor      | `Ctrl+S` / `Ctrl+W`                    | save / close this tab                     |
| editor      | `Ctrl+F` / `Ctrl+H`, `F3` / `Shift+F3` | find / replace, next / previous           |
| editor      | `Ctrl+/`, `Ctrl+D`, `Ctrl+Shift+K`     | comment lines, duplicate, delete them     |
| editor      | `Alt+↑` / `Alt+↓`                      | move lines up / down                      |
| editor      | `Ctrl+Z` / `Ctrl+Y`, `Ctrl+Shift+Z`    | undo / redo                               |
| editor      | `Ctrl+Space` / `Ctrl+Shift+Space`      | completion / parameter hints              |
| editor      | `F12`, `Ctrl+Click` / `Alt+←`          | go to definition / back                   |
| editor      | `Shift+F12` / `F2`                     | find all references / rename symbol       |
| editor      | `Shift+Alt+F`                          | format the file with clangd               |
| editor      | `Ctrl+.` / `Ctrl+Shift+.` / `Alt+O`    | quick fix / go to a symbol in the file / switch source and header |
| editor      | `Ctrl+Shift+[` / `Ctrl+Shift+]`        | fold / unfold the block                   |
| editor      | right click                            | quick fix, go to definition, references, symbols, rename, format, comment, breakpoint, watch, compare, history |
| editor      | double click                           | select a name (its other occurrences light up) |

### What else is there

**Editing and Crash recovery.** A file with changes is saved a second after the last one (the
autosave setting), so `Ctrl+S` is rarely needed; a tab shows `●` while its file has unsaved changes,
and closing it then asks whether to save. Unsaved changes are also copied to
`~/.cache/t-code/recovery/` a moment after each edit: if t-code or the system stops unexpectedly,
opening the file again shows a bar that restores them (or discards the copy). A file changed on disk (a `git checkout`, another program) is loaded again when the
editor has nothing unsaved.

**Problems panel (`Ctrl+Shift+M`).** The language servers' errors, warnings and notes in every open
file, in one list with their counts on top and `file:line:column` beside each; a click goes there.
It sits in the bottom dock and moves like any other panel.

**Project tasks.** A `.t-code/tasks.json` (or `t-code.json`) in the project tells Build, Run, Test
and F5 what to run (with arguments, environment, working folder, and named profiles such as
`debug` / `release`), instead of t-code guessing; the commands run in the shell. See
[`docs/tasks.md`](docs/tasks.md).

**Status bar**: the git branch (`*` when there are changes), the language servers' errors and
warnings in the open files, **⚠ No compile flags** when clangd lacks them, **⚙ Build** and
**▶ Run** with what they would build (a plugin's or recovery's short messages show there too), the file's indentation (**Spaces: 4**, **Tab Size: 8**), the
cursor's line and column, and **⌨ Keybinds**.

**Indentation.** Each file gets the indentation its project asks for: the Settings' tab size and
spaces, overridden by what the file itself uses (detect indentation), then by the project's
`.editorconfig` and `.clang-format` (for C and C++), and last by a choice made for the project.
The status bar shows the indentation in use and where it comes from; clicking it (or "Change
Indentation for This Project…" in the command palette) picks one for the whole project, which is
remembered. Makefiles always get tabs.

**Debugging.** `F5` saves, builds with debug info and starts the program under gdb (14 or newer,
`gdb -i dap`); a click left of a line number (or `F9`) puts a breakpoint there, a right click asks
for a condition. When it stops, the line is marked, and the Run and Debug panel shows the
variables (the ones that changed in color, arrays and structs opened like folders), watch
expressions, the call stack and every breakpoint; the pointer on a name shows its value. The Debug
panel at the bottom has the buttons (continue, pause, step over / into / out, restart, stop), the
program's own terminal and a line to evaluate expressions (or `-exec` gdb commands). A crash
(segfault, AddressSanitizer) stops on the bad line and says in one sentence what happened. **ASM**
opens the Disassembly View: the function's instructions with its source lines, stepping by
instruction and instruction breakpoints. A Makefile's program is picked once and remembered.

**Build and run.** `Ctrl+F5` / Run saves, then builds and runs in the shell (Build stops after
building), looking upwards from the current file: CMake uses `CMakeLists.txt`, Meson uses
`meson.build`, and Make uses a Makefile; otherwise the current C or C++ file is compiled on its own
(with AddressSanitizer and UBSan) into `build/` and run. Ctrl+click a `file.c:12:5` in compiler
output to go there.
Other languages run too: a script by its interpreter (Python, JavaScript / TypeScript with node,
shell, Lua, Ruby, Perl, PHP, Go, Java, or any file with `#!`), and projects by their files: Cargo,
`go.mod`, `package.json`'s `start` / `dev` script, Maven, Gradle, Python from its root with its venv.

**Test.** The command palette's **Test** saves changed files and runs the active task profile's
`test` command, or detects CMake/CTest, Meson, `make test`, Cargo, Go, Node's `test` script, Maven,
Gradle or `pytest` in a Python project. Output stays in the integrated shell; Ctrl-click a
`file:line:column` location to open the failing source.

**Git.** The editor marks changed lines in the gutter (blue changed, green added, red
where lines were deleted), following new commits by itself. **Compare with HEAD** (`Ctrl+Alt+D`)
and **File History** (`Ctrl+Alt+H`, pick a commit) open one tab ("file ↔ commit") with the file
on the left and the old version read-only on the right, scrolling together and lined up (also with
word wrap): changed lines are tinted, their changed words colored (green new, red old), and gap
rows stand for missing lines; it opens at the first change. Closing the tab closes both sides; "Git: Stop Comparing" in the
command palette keeps the file, back to comparing with the last commit. The Files panel colors files by their git state (modified,
added, untracked, conflicted, ignored) with the letter at the end of the row. The **Source
Control** panel (`Ctrl+Shift+G`) commits, pushes, stages and unstages changes, and discards after
confirmation. Right-click a modified file to stage or unstage an individual hunk. It also lists
commits: the pointer on one shows its message, author, date and line counts, and "Open Commit Details"
shows it with its diff.

**Files panel.** A folder is read when you open it and shows new, removed and renamed files by
itself; the git colors follow commits and saves. Files and folders can be cut, copied and pasted
(a name that is taken gets " copy"), dragged onto a folder (from another file manager they are
copied), or dragged onto the editors to open them. A new file opens in the editor.
A click opens a file in a temporary tab (its name in italics) that the next click
replaces, so browsing does not pile up tabs; editing the file or double-clicking it (in the tree or
on the tab) keeps it, and a middle click opens it to the side.

**Split editor.** `Ctrl+\` (or the title bar button) puts a second group of tabs next to the
first. New files open in the group you are in; "Open to the Side" in the Files panel opens a
file straight into the right group. Tabs can be dragged between the groups, and the right group
closes when its last tab does. A file is never open in both groups at once, since two editors
saving the same file would overwrite each other.

**Second window.** "Open in New Window" (the Files panel's menu, a tab's menu) moves a file into a
second window of the same project, sharing its language servers; closing it moves its tabs back.

**Projects.** The title bar has buttons to open another folder and to pick a recent one.
For a second project next to the first, middle-click the dock icon (or pick "New Window" from
its menu, or run `t-code --new-window`): a new window opens and asks for a folder.
Switching remembers and closes the current project's tabs, shows the new folder in the Files
panel, restarts the shell there, and brings back that folder's tabs from last time. t-code also remembers
the window size, pane sizes, hidden panes and text size (in `~/.config/t-code/session.ini`).

**Search** (`Ctrl+Shift+F`), a tab next to Files and Source Control: results
come as you type, grouped by file with the matches marked. **Aa**, **ab** and **.\*** next to the
text turn on Match Case, Match Whole Word and regular expressions; **⋯** shows "files to include"
and "files to exclude" (comma separated globs such as `*.c, src`). Click a file to fold its
results, a result to open the file there. ripgrep skips what `.gitignore` ignores; without it,
grep searches everything but `.git` and `build`.

**Color themes.** Dark Classic and other dark and light themes (Gruvbox among them), each with a
preview, in a rounded or a square look; the editor, the terminals and the panels all follow.
`theme.css` and the editor's style scheme can be edited by hand ("Preferences: Open …").

**Settings** (`Ctrl+,` or the gear in the title bar, its pages listed on the left): minimap, code coloring, indent guides,
word wrap (off by default; with it off, long lines run off the screen and scroll sideways with
the horizontal scrollbar or Shift + wheel), tab size (2 by default), spaces for Tab, detecting a file's own indentation, autosave,
format on save (`Ctrl+S` formats C and C++ first; off by default), clang-tidy checks (the project's
`.clang-tidy`; off by default), checking for updates (the latest release on GitHub against this
one, asking once when a newer one is out; "Check for Updates Now" from the palette asks at any
moment), fonts, text size and reopening the last project. Changes apply at
once, to the open editors too, and are kept in `~/.config/t-code/settings.ini` (a comment over
each key; a hand edit applies at once).

**Markdown preview** opens as a tab and follows the file as you type. It is drawn natively
(md4c into a GTK text view, no browser engine): headings, lists, quotes, code blocks colored like
the editor, tables whose cells wrap, local images. Links to other markdown files open their
preview, other local files open in an editor, and web links open in the browser.

## Performance

t-code is meant to be light: GtkSourceView editors and the folder tree in one process, and one
language server per language that only knows the files you have looked at.

Measured on Fedora 44 (x86_64) with `tools/headless.sh` (t-code run invisibly, then
`tools/proc-report.sh`), the same 25 C files of this repository opened, each tab then shown in
turn, against the micro-based 0.1.1 and 0.2 run the same way. Memory is PSS: pages shared between processes
are split between them, so the rows add up.

| all 25 tabs shown | 0.1.1 (micro) | 0.2 (GtkSourceView) | 0.3 |
|---|---:|---:|---:|
| editors | 25 micro processes, 412 MB | in the window | in the window |
| t-code window | 57 MB | 83 MB | 70 MB |
| helper processes per editor | 50, 6 MB | none | none |
| clangd | 361 MB | 470 MB | 345–367 MB |
| **memory in all** | **839 MB** | **557 MB** | **418–442 MB** |

0.3 (two runs, the shell's 3 MB included) adds the debugger, folding, sticky scroll, themes and
the rest without growing: gdb only runs while debugging, and the window came out smaller than
0.2's. clangd's share varies the most between runs (see below). At rest, t-code took
0.2 % of one core over 5 s.

clangd's share moves a lot between runs (its background index, what it has freed back); the rest
is steady. With 25 tabs open and only one shown, both come to about 60 MB besides clangd: 0.1.1
started micro (about 20 MB) for each tab shown, an editor now costs its file and a few MB in the
window. While idle, t-code uses no CPU: nothing polls, editors redraw only when something changes.

What keeps it small:

- **clangd gets a file once its tab is shown**, and stops a minute after the last C or C++ file
  closes. It keeps at most five files open: the least recently used hidden tab is closed on the
  server (its AST freed) and opened again when you go back to it.
- **clangd's own flags**: precompiled headers on disk (`--pch-storage=disk`), two indexing
  threads, at most 50 results, and `--malloc-trim` on glibc.
- **clangd runs with two malloc arenas** (`GLIBC_TUNABLES=glibc.malloc.arena_max=2`). With 25
  files clangd itself accounted for 130 MB, yet held 1.5 GB: glibc's per-thread arenas kept
  memory it had freed. With two arenas it stays at a third of that, at the same speed.
- **Only what changed goes to clangd** (incremental sync), batched while typing.
- **No browser engine**: the Markdown preview is a text view, not WebKit (which started a web and
  a network process, 150-200 MB, per preview).
- **Freed memory goes back**: t-code calls glibc's `malloc_trim(0)` when a tab closes, a search
  ends and the project changes.
- **The Files panel reads only open folders** and frees a folder's rows when it closes. It does
  not watch build output (`build/`, `target/`, `node_modules/`, `.git/`, `.cache/`), and a burst of
  changes reads a folder once (250 ms). Saving a file reads nothing: only files coming, going or
  being renamed do.

How to measure it yourself (see `tools/`):

```sh
tools/proc-report.sh 10              # the running t-code: processes, memory and CPU by program
tools/headless.sh src/*/*.c          # build/t-code run invisibly with these files, then the same report
tools/clangd-mem.py --churn 3        # clangd alone: resident memory vs what it says it uses
make test-asan                       # the tests without GTK under AddressSanitizer, failing on any leak
```

The tests also check memory: `tests/test_lsp.c` opens and closes a file 200 times against a fake
language server and fails if the heap grows by 8 KB (it grows by a few hundred bytes; a leak of 130
bytes a round fails it), and the package tests check that one clangd serves all C/C++ editors with
the arena limit.

## Files

```
src/                 t-code, the window (src/app.h: the shared types and a map of the folders;
                     every .c file has a .h next to it with what it offers the others)
  main.c             start and quit: the command line, the application, signals
  window/            the window: layout and panes (window.c), where its data is (paths.c),
                     theme, terminals and Ctrl+click on compiler messages (terminal.c), the status
                     bar, every command: shortcuts, palette and Keybinds list (commands.c)
  editor/            editor tabs and the two groups (tabs.c), opening, saving, autosave and closing
                     (editor.c), the page with the minimap and its look (editorview.c), editing
                     commands and typing help (editing.c), the find bar (findbar.c), indent guides
                     (guides.c), the selection highlight (occurrences.c), the git diff gutter
                     (diffgutter.c), a file's indentation and language (detect.c), what
                     .editorconfig and .clang-format say about it (indentconf.c); markdown preview
                     (preview.c), crash recovery (recovery.c)
  lsp/               the language servers: the client and each server's lifetime (client.c, over
                     channel.c), diagnostics and the Problems panel (diagnostics.c, problems.c), hover,
                     completion, parameter hints (signature.c), definition / references / rename / format
                     (navigate.c, edits.c), colors by meaning (semantic.c), finding the compile database (compiledb.c)
  plugins/           plugins: finding their manifests, palette commands and shortcuts, running them (plugins.c)
  search/            the Search tab, Ctrl+Shift+F (ripgrep); searchmatch.c is its GLib-only part
  git/               the Source Control tab (scm.c), per-hunk patches (patch.c), git status in the
                     background (gitstatus.c), comparing with HEAD or history (compare.c), line
                     differences for the gutter (linediff.c)
  popups/            the popup list (picker.c) used by Ctrl+P (quickopen.c), the palette, history,
                     references and rename
  dialogs/           the Settings, Keyboard Shortcuts and About windows
  project/           switching folders (project.c), remembering the window and tabs (session.c),
                     build, run, test and project tasks (run.c)
  files/             the Files panel (files.c): a folder's entries read and watched (folder.c), file
                     operations (fileops.c), git colors (gitmarks.c), file icons (icons.c)
  update/            the new-release check: GitHub's latest release against this one (update.c)
  ipc/               requests from scripts and tools over $TCODE_SOCKET (socket.c)
styles/              the editor's colors: Dark Classic as a GtkSourceView style scheme
plugins/             word-count, an example plugin (found when t-code runs from the source tree)
docs/                plugins.md (manifest and socket protocol), tasks.md, memory.md, screenshots
tests/               make test: editing and the find bar, the language server client (with a fake
                     server), detect.c and indentconf.c, linediff.c, searchmatch.c, the Files panel
                     and its GIO half, the Search panel, Source Control and hunk patches, tabs in a
                     whole window, About, plugins, the update check and crash recovery; make
                     test-asan: the ones without a display, plus plugins, update and recovery, under ASan
tools/               measuring memory and CPU: the running process tree, a headless t-code, clangd
desktop/             app menu entry template and the app icon
packaging/           install.sh, uninstall.sh, build.sh, test.sh: Fedora (fedora/t-code.spec), Arch
                     (arch/PKGBUILD), AUR, Debian (debian/debian/), Void Linux (void/), Flatpak,
                     Snap and AppImage packages, built into packaging/packages/ (not in git)
.clang-format        the code's style (2 spaces); clangd formats with it
LICENSE
Makefile             VERSION, make install; also writes build/compile_commands.json, so clangd
                     understands this code
```

## License

MIT, see [LICENSE](LICENSE). The color themes' sources and their licenses are in
[THIRD-PARTY.md](THIRD-PARTY.md).

## Contributors

- [@suacayipkisi](https://github.com/suacayipkisi) — Creator & Lead Maintainer
- [@dontworryplz](https://github.com/dontworryplz) — Plugin system, unified problems panel, tasks runner, crash recovery, Flatpak, Snap & AUR packaging
