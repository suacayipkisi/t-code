# <img src="desktop/io.github.tcode.svg" alt="" width="48" align="top"> t-code

One window, three kinds of terminals, nothing else.

> **Branch `micro-main`:** this is t-code with [micro](https://micro-editor.github.io) as the editor,
> as released in 0.1.1. It stays here as it is and gets fixes only. New work goes to `main`, where
> the editor is being replaced by [GtkSourceView 5](https://gitlab.gnome.org/GNOME/gtksourceview)
> (see `docs/gtksourceview-plan.md` on `main`).

**Version 0.1.1** — uses about 8 times less memory than 0.1 and almost no CPU while idle
([Performance](#performance)); packages for Fedora, Arch Linux, Debian and Void Linux (glibc and
musl) ([Install](#install)).
`t-code --version` prints the installed version.

![t-code editing its own source: the file manager on the left, src/t-code.c open in the editor with indent guides and the minimap, the shell below, and the status bar](docs/screenshot.png)

<sub>t-code open on its own repository: the file manager with the <code>src/</code> tree expanded, <code>src/t-code.c</code> in the editor with indent guides (the block around the cursor highlighted) and the minimap next to the scrollbar, the shell below, and the status bar with the git branch, clangd's errors and warnings, Build / Run and the cursor position.</sub>

Two small C programs for Linux. With 25 files open it uses about 275 MB of RAM in all, clangd
included ([Performance](#performance)). `t-code` (GTK4) is the window; every pane in it is a real terminal (VTE, the
same widget GNOME's terminals use):

- **left:** `t-code-fm`, a file manager written in plain C (no libraries)
- **right:** editor tabs, each one its own terminal running [micro](https://micro-editor.github.io)
  with clangd checking the code (micro is choosen because vim/neovim usage is hard for new users)
- **bottom:** your shell

All terminals use Cascadia Code and VS Code's Dark Modern colors. C and C++ code is colored the
way VS Code with clangd colors it (`micro/colorschemes/dark-modern.micro`, `micro/syntax/c.yaml`,
`micro/syntax/cpp.yaml`): the colors come from VS Code's own theme files, so functions are yellow,
types teal, variables light blue, macros blue, enum constants bright blue, control flow
(`if`, `return`, `#include`) purple, and comments, control flow and type keywords (`int`,
`static`, `const`, `class`) are italic. clangd tells these apart by meaning; t-code tells them
apart by the shape of the name (`GtkWidget` and `std::vector` are types, `FOO(...)` is a macro,
`TAB_EDITOR` an enum constant), which is right almost always but not every time. VS Code's
colored bracket pairs are not possible in micro. `.h` files open as C++ when they contain
C++ (`class`, `namespace`, `std::`, ...).

micro runs with its own config in `micro/` (your `~/.config/micro` is untouched). Its only plugin,
the LSP client for clangd, is copied into `micro/plug/lsp` at a fixed version, so nothing downloads
or updates by itself.

## Requirements

`micro` (2.0.14 or newer), `clangd`, `git`, the Cascadia Code font (`ripgrep` is optional, for a faster Search
panel), and to build: `gcc` 14 (C23), `make` and the GTK4, VTE, WebKitGTK and md4c
headers. The packages below pull in all of these by themselves.

```sh
sudo dnf install gtk4-devel vte291-gtk4-devel webkitgtk6.0-devel md4c-devel
```

## Install

### With a package (Fedora, Arch Linux, Debian, Void Linux)

t-code 0.1.1 has packages for Fedora 44, Arch Linux, Debian 13 (trixie) and Void Linux (glibc and
musl), x86_64. Install the one for your distribution; the package manager pulls in micro, clangd,
git and the Cascadia Code font:

```sh
sudo dnf install ./t-code-0.1.1-1.fc44.x86_64.rpm        # Fedora
sudo pacman -U t-code-0.1.1-1-x86_64.pkg.tar.zst         # Arch Linux
sudo apt install ./t-code_0.1.1-1_amd64.deb              # Debian
```

On Void Linux, xbps installs from a repository, so the downloaded package is indexed first (in
the folder it is in; use the `x86_64-musl` file on musl):

```sh
xbps-rindex -a t-code-0.1.1_1.x86_64.xbps
sudo xbps-install --repository="$PWD" t-code
```

Void has no package for Cascadia Code: t-code then uses the system's monospace font, and any
font can be picked in Settings. The Void packages were built and tested in Void containers.

Then start `t-code` from the app menu, or `t-code [folder]` in a terminal. To remove it, use
your package manager (`sudo dnf remove t-code`, `sudo pacman -R t-code`, `sudo apt remove t-code`,
`sudo xbps-remove t-code`).

The package puts `t-code` in `/usr/bin`, the file manager and `t-code-lsp` in
`/usr/libexec/t-code/` (`/usr/lib/t-code/` on Arch), the editor config in
`/usr/share/t-code/micro/`, and the app menu entry and icon. Each user gets their own editor
folder in `~/.local/state/t-code/micro/`, where micro keeps its backups and history; it links to
the shared config, so package updates reach it.

### Building the packages yourself

All packages are built from this repository with one script. The Arch, Debian and Void packages
(and the Fedora one, when you are not on Fedora) are built in containers, so all you need is
`podman` or `docker`:

```sh
packaging/build.sh                # all, or pick: packaging/build.sh fedora arch debian void void-musl
packaging/test.sh                 # tries each package out in a fresh container
```

The packages land in `packaging/packages/<distribution>/` (with the debug and source packages);
`packaging/packages/release/` gets just the ones to publish and their `SHA256SUMS`. On Fedora, the RPM is
built directly with `rpmbuild`; install its build dependencies once with
`sudo dnf install rpm-build dnf-plugins-core && sudo dnf builddep packaging/fedora/t-code.spec`.

`packaging/test.sh` installs each package in a fresh container of its distribution (so the
package manager has to find every dependency), checks the installed files, and starts t-code on
a headless X server with a C file: the file manager, micro, `t-code-lsp` and clangd have to come
up, t-code has to quit cleanly, and micro has to load its plugins and save a file. While they
build, the packages also run the tests that need no display.

### From the source, into ~/.local

```sh
./install.sh
```

This builds t-code and installs it under `~/.local`: the `t-code` command, the file manager in
`libexec/t-code/`, the editor config in `share/t-code/micro/`, and a "t-code" entry in the app menu
with its own icon (`desktop/io.github.tcode.svg`, installed to `~/.local/share/icons`). Run it again after changing the code to update the installed copy.

To remove everything it installed:

```sh
./uninstall.sh
```

`make install` installs system-wide instead (`PREFIX=/usr/local` by default, `DESTDIR` for
staging; the packages use it). For development, `make` builds into `build/`, and `build/t-code`
runs straight from the repo.
`make test` runs the tests in `tests/` (the Search panel, minimap and Source Control ones open a
window, so they need a display). `make test-asan` runs the ones without a window again under
AddressSanitizer, which also fails on memory leaks. `tools/` has scripts that measure what a
running t-code costs in memory and CPU (`tools/proc-report.sh`), or run it invisibly to measure a
change (`tools/headless.sh`).

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
| anywhere    | `F5` / `Ctrl+Shift+B`                  | build and run / build only (see below)    |
| anywhere    | `Ctrl+Alt+D` / `Ctrl+Alt+H`            | compare with the last commit / with a commit from the file's history |
| anywhere    | `Ctrl+Shift+V`                         | markdown preview of the current file      |
| anywhere    | `Ctrl+Shift+O`                         | open another project folder               |
| anywhere    | `Ctrl+\`                               | split the editor into two groups / join   |
| anywhere    | `Ctrl+1` / `Ctrl+2`                    | left / right editor group                 |
| anywhere    | `Ctrl+PageDown` / `Ctrl+PageUp`, `Alt+.` / `Alt+,` | next / previous tab           |
| anywhere    | `Alt+1` / `Alt+2` / `Alt+3`            | file manager / editor / shell             |
| anywhere    | `Ctrl+B` / `Ctrl+J`                    | hide / show the file manager / the shell  |
| anywhere    | `F7`                                   | code coloring on / off (all tabs)         |
| anywhere    | `Ctrl+=` / `Ctrl+-` / `Ctrl+0`         | bigger / smaller / normal text            |
| shell       | `Ctrl+Click` on `file.c:12:5`          | open the file at that line (compiler messages) |
| tab         | left click / middle click / `×`        | switch to / close / close                 |
| files       | click, `Enter`, `l`, `→`               | open folder, or open the file in a tab    |
| files       | `h`, `Backspace`, `←`                  | collapse folder / go to parent            |
| files       | `↑` `↓`, `j` `k`, wheel                 | move / scroll                             |
| files       | `a` / `r` / `d`, `Delete`              | new file (end with `/` for a folder) / rename / delete |
| files       | `Ctrl+X` / `Ctrl+C` / `Ctrl+V`         | cut / copy / paste files and folders      |
| files       | drag a row onto a folder               | move it there                             |
| files       | `.` / `R`                              | hide / show dotfiles (shown by default) / refresh |
| files       | right click                            | Open, Open to the Side, Open Preview, Compare with HEAD, File History, New File/Folder, Open in Terminal, Cut/Copy/Paste, Copy (Relative) Path, Rename, Delete, Open Folder as Project |
| editor      | `Ctrl+S` / `Ctrl+Q`                    | save / close this tab                     |
| editor      | `Ctrl+F`, `F3` / `Shift+F3`            | find, next / previous                     |
| editor      | `Tab`, `Ctrl+Space`                    | completion                                |
| editor      | `Alt+K`                                | signature of the symbol under the cursor  |
| editor      | `Alt+D` / `Alt+R`                      | go to definition / list references        |
| editor      | `Alt+F`                                | format the file with clangd               |

### What else is there

**Editing.** Files with changes are saved every second (the autosave setting), and closing a tab
saves too, so `Ctrl+S` is rarely needed; a tab shows `●` while its file has unsaved changes.
clangd marks problem lines with `>>` in the gutter, and the minimap shows them as red (errors)
or yellow (warnings) lines across its width; move the cursor onto the line to read the message at
the bottom. Everything else in the editor is plain micro (`Ctrl+C`/`Ctrl+V`, `Ctrl+Z`,
...; `Ctrl+E` opens its command bar, `help` there shows its help).

**Status bar**, like VS Code's: the git branch (`*` when there are changes), clangd's errors and
warnings in the open files, **⚙ Build** and **▶ Run** with what they would build, the cursor's
line and column, and **⌨ Keybinds**.

**Build and run.** F5 / Run saves, then builds and runs in the shell (Build stops after
building), looking upwards from the current file: a `CMakeLists.txt` is configured and built in
`build/` and the newest program there runs; a `Makefile` runs `make`, then `make run` if there is
a `run` target; otherwise the current C or C++ file is compiled on its own (with AddressSanitizer
and UBSan) into `build/` and run. Ctrl+click a `file.c:12:5` in the compiler's output to go there.

**Git.** The editor marks changed lines in the gutter like VS Code (blue changed, green added, red
where lines were deleted), following new commits by itself. **Compare with HEAD** (`Ctrl+Alt+D`)
and **File History** (`Ctrl+Alt+H`, pick a commit) open the old version read-only to the side:
both gutters then mark the differences between the two. Closing the old version goes back to
comparing with the last commit. The file manager colors files by their git state (modified,
added, untracked, conflicted, ignored) with the letter at the end of the row.

**File manager.** It refreshes by itself when files are added, removed or saved, and after a git
commit. Files and folders can be cut, copied and pasted (a name that is taken gets " copy"), or
dragged onto a folder.

**Split editor.** `Ctrl+\` (or the title bar button) puts a second group of tabs next to the
first. New files open in the group you are in; "Open to the Side" in the file manager opens a
file straight into the right group. Tabs can be dragged between the groups, and the right group
closes when its last tab does. A file is never open in both groups at once, since two editors
saving the same file would overwrite each other.

**Projects.** The title bar has buttons to open another folder and to pick a recent one.
For a second project next to the first, middle-click the dock icon (or pick "New Window" from
its menu, or run `t-code --new-window`): a new window opens and asks for a folder.
Switching remembers and closes the current project's tabs, restarts the file manager and the
shell in the new folder, and brings back that folder's tabs from last time. t-code also remembers
the window size, pane sizes, hidden panes and text size (in `~/.config/t-code/session.ini`).

**Search** (`Ctrl+Shift+F`), a tab next to Files and Source Control, like VS Code's: results
come as you type, grouped by file with the matches marked. **Aa**, **ab** and **.\*** next to the
text turn on Match Case, Match Whole Word and regular expressions; **⋯** shows "files to include"
and "files to exclude" (comma separated globs such as `*.c, src`). Click a file to fold its
results, a result to open the file there. ripgrep skips what `.gitignore` ignores; without it,
grep searches everything but `.git` and `build`.

**Settings** (`Ctrl+,` or the gear in the title bar): minimap, code coloring, indent guides,
word wrap (off by default; long lines then continue on the next row instead of running off the
screen), tab size, spaces for Tab, detecting a file's own indentation, autosave, fonts, text size
and reopening the last project. Changes apply at once and are kept in `session.ini`.

**Markdown preview** opens as a tab and follows the file as you type. Links to other markdown
files open their preview, other local files open in an editor, and web links open in the browser.

## Performance

t-code is meant to be light: a few small C programs around micro and clangd. Version 0.1.1 was
measured against 0.1 on the same machine (Fedora 44, x86_64), both built from source and run
with the same 25 C files of this repository, after clangd had finished indexing.

**25 files open, one tab looked at** (the usual case):

| | 0.1 | 0.1.1 | |
|---|---:|---:|---:|
| clangd | 1718 MB | 208 MB | −88% |
| editors (micro) | 25 running, 466 MB | 1 running, 19 MB | −96% |
| t-code window | 46 MB | 44 MB | |
| helper processes | 50, 6 MB | 2, 0.2 MB | |
| **memory in all** | **2239 MB** | **275 MB** | **−88%** |
| **CPU while idle** (one core) | **120%** | **under 3%** | |
| first editor ready | 0.93 s | 1.01 s | |

**All 25 tabs opened one after another** (the worst case: every editor running):

| | 0.1 | 0.1.1 |
|---|---:|---:|
| clangd | 1718 MB (peak 1760 MB) | 433 MB (peak 482 MB) |
| editors (micro) | 466 MB, 84% CPU | 442 MB, 0% CPU |
| **memory in all** | **2254 MB** | **942 MB** |
| **CPU while idle** | **130%** | **under 3%** |

In a normal desktop session with 25 tabs (Wayland, GNOME) 0.1.1 used 232 MB in all and 0.3% of
one core while idle. Memory is PSS: pages shared between processes are split between them, so
the rows add up.

What changed:

- **An editor starts when its tab is first shown.** A restored session with many tabs runs micro
  only for the visible ones; each one costs about 20 MB, a connection to clangd and its file in
  clangd's memory.
- **clangd runs with two malloc arenas** (`GLIBC_TUNABLES=glibc.malloc.arena_max=2`). With 25
  files clangd itself accounted for 130 MB, yet held 1.5 GB: glibc's per-thread arenas kept
  memory it had freed. With two arenas it stays around 700 MB in the worst case, also after
  hours of editing, at the same speed. (jemalloc did not help.)
- **Idle editors write nothing.** In 0.1 a timer in the editor plugin re-armed itself every 30 ms
  and every editor redrew its screen about 35 times a second, which the window then had to read.
  Now a shown editor redraws once a second, a hidden one not at all, and autosave is done by
  t-code for the files that changed instead of by a timer in every editor.
- **The Markdown preview's network process** (about 60 MB) exits with the last preview.
- **No clipboard windows at startup:** each editor's clipboard check used to flash a window in
  GNOME's dock and could clear the clipboard.

How to measure it yourself (see `tools/`):

```sh
tools/proc-report.sh 10              # the running t-code: processes, memory and CPU by program
tools/headless.sh src/*.c            # build/t-code run invisibly with these files, then the same report
tools/clangd-mem.py --churn 3        # clangd alone: resident memory vs what it says it uses
tools/micro-redraws.py               # an idle editor's screen writes per second, shown and hidden
make test-asan                       # the tests under AddressSanitizer, failing on any leak
```

The tests also check memory: `tests/test_lsp.c` connects and disconnects 400 editors to the shared
language server and fails if the heap grows by 32 KB (it grew by under 1 KB), and the package
tests check that 5 opened files start only 1 editor and that clangd has the arena limit.

## Files

```
src/                 t-code, the window (src/app.h: the shared types and a map of the folders;
                     every .c file has a .h next to it with what it offers the others)
  main.c             start and quit: the command line, the application, signals
  window/            the window: layout and panes (window.c), where the helpers are (paths.c),
                     theme, terminals and Ctrl+click on compiler messages (terminal.c), the status
                     bar, every command: shortcuts, palette and Keybinds list (commands.c)
  editor/            editor tabs and the two groups (tabs.c), micro in a tab (editor.c), the page
                     around it (editorview.c): the control pipe to micro (control.c), the minimap
                     (minimap.c, lexer.c) and the indent guides (guides.c); markdown preview
  search/            the Search tab, Ctrl+Shift+F (ripgrep); searchmatch.c is its GLib-only part
  git/               the Source Control tab (scm.c), git status in the background (gitstatus.c),
                     comparing with HEAD or a commit from the history (compare.c)
  popups/            the popup list (picker.c) used by Ctrl+P (quickopen.c), the palette and history
  dialogs/           the Settings, Keyboard Shortcuts and About windows
  project/           switching folders (project.c), remembering the window and tabs (session.c),
                     build and run (run.c)
  ipc/               requests from the file manager over $TCODE_SOCKET (socket.c), the shared
                     language servers (lsp.c, with lspchannel.c and lspjson.c)
  fm/                t-code-fm, the file manager (see src/fm/fm.h)
  lsp/               t-code-lsp, connects an editor's lsp plugin to the shared servers
tests/               make test: searchmatch.c, the shared language servers (ipc/lsp.c), the Search
                     panel, the minimap, Source Control and About; make test-asan: the first two
                     under ASan
tools/               measuring memory and CPU: the running process tree, a headless t-code, clangd,
                     idle editors
micro/               editor config: settings, colors, C and C++ syntax, the clangd plugin, and the tcode
                     plugin that reports the editor's state to t-code and sets the git comparison
desktop/             app menu entry template and the app icon
packaging/           build.sh, test.sh: Fedora (fedora/t-code.spec), Arch (arch/PKGBUILD), Debian
                     (debian/debian/) and Void Linux (void/) packages, built into packaging/packages/
                     (not in git)
install.sh, uninstall.sh
LICENSE
Makefile             VERSION, make install; also writes build/compile_commands.json, so clangd
                     understands this code
```

## License

MIT, see [LICENSE](LICENSE). The clangd plugin in `micro/plug/lsp` is by Robert Kunze, also MIT
(its own LICENSE is next to it).
