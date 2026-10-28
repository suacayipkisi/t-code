# Features

What t-code does, in detail. The [README](../README.md) has the short version.

## The editor

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
  `#include`, a misspelled name) and its refactorings, one pick away; resting the pointer on a
  problem shows its fixes as buttons (`Quick Fix: #include <iostream>`) under the message. Other
  servers' fixes come the same way; pages a server shows (gopls' package documentation and
  assembly) open in the browser.
- **Inlay hints:** parameter names before arguments (`add(left: 1, right: 2)`) and deduced types
  after names (`auto item: Widget = ...`), dim, inside the line; the text itself does not change.
  From clangd, rust-analyzer, gopls and the TypeScript server, for the shown lines; the "Inlay hints"
  setting turns them off. Holding `Ctrl+Alt` (alone) turns them the other way while held: shows them
  when they are off, hides them when they are on.
- **Outline and breadcrumbs:** the **Outline** panel (Panels menu, or "View: Toggle Outline") lists
  the current file's classes, functions and fields as the language server sees them, the one around
  the cursor selected; a click goes there. Over each editor, the breadcrumbs show the file's folders
  and the symbols around the cursor (`src › shape.cpp › Shape › draw`); a symbol's crumb goes to it.
  The "Breadcrumbs" setting hides them.
- **Call and type hierarchy:** `Shift+Alt+H` (or the editor's right-click menu) on a function shows
  who calls it in the **Hierarchy** panel, as a tree that opens further as you go; **Callers** /
  **Callees** switch the direction. "Show Type Hierarchy" does the same for a type's supertypes and
  subtypes. A row opens its place. clangd, gopls and rust-analyzer offer calls; clangd also types.
- **Code folding** (the arrow beside a block, `Ctrl+Shift+[` / `]`) and **sticky scroll**: the
  heads of the function, loop or namespace you are in stay at the top of the editor.
- **Colors by meaning**: clangd tells functions (yellow), types (teal), macros (blue), constants and
  enum members (bright blue) and variables (light blue) apart; GtkSourceView colors keywords,
  strings, comments and the rest from its theme (`styles/dark-classic.xml`). `.h` files
  open as C++ when they contain C++.
- Find and replace (`Ctrl+F`, `Ctrl+H`; case, whole word, regular expressions), go to line
  (`Ctrl+G`), comment lines (`Ctrl+/`), duplicate (`Shift+Alt+↓`), move (`Alt+↑` / `Alt+↓`) and
  delete (`Ctrl+Shift+K`) lines, brackets and quotes that close by themselves (and wrap a selection).
- **Snippets:** type a trigger (`main`, `for`, `if`, `def`, `iferr`, `match`, `af`, ...) and `Tab`;
  `Tab` / `Shift+Tab` go through its fields. The completion list offers them too. t-code brings
  some for C, C++, Python, Go, Rust, JavaScript / TypeScript and shell; your own go in
  `~/.config/t-code/snippets/` ("Preferences: Open User Snippets"), read when t-code starts.
- **Multiple cursors:** `Ctrl+D` selects the name at the cursor, then adds its next occurrence;
  `Ctrl+Shift+L` selects every occurrence; `Ctrl+Shift+↑` / `↓` add a cursor on the line above /
  below; `Alt+Click` adds (or removes) one; `Shift+Alt+drag` selects a column. Typing, Backspace,
  Delete, Enter, Tab, the arrows, Home / End and copy / cut / paste act on all of them as one undo
  step (pasting as many lines as there are cursors gives one line each); `Esc` or a click goes back
  to one.
- `Enter` after an opening bracket goes one level deeper (between a pair the closer gets its own
  line), and `}` outdents. A double click selects a name only (letters, digits, `_`).
- **Highlights**: the cursor on a name lights up that symbol's uses (clangd's, not other names
  spelled the same); selecting a name lights up its text everywhere. Their lines are marked in the
  strip beside the minimap, as are git's changes. Code an `#if` leaves out is dimmed.
- **Right-click menus** on the text, tabs, files, Source Control's changes and commits, search
  results, terminals and the debugger's lists.
- A minimap, indent guides (the block around the cursor in its bracket color), the git diff gutter
  (green added, blue changed, red deleted), autosave a second after the last change.

clangd starts when the first C or C++ file is shown; Python (`.py`), Go (`.go`), Rust (`.rs`) and
JavaScript / TypeScript (`.js`, `.jsx`, `.mjs`, `.cjs`, `.ts`, `.tsx`, `.mts`, `.cts`) files use `pylsp`,
`gopls`, `rust-analyzer` and `typescript-language-server` (`npm install -g typescript-language-server
typescript`) respectively when the executable is on `PATH`
(`pylsp` also from the project's venv). **Python: Select Interpreter…** (command palette) picks the
project's Python among its venvs, `$VIRTUAL_ENV`'s and `python3`; **Rust: Select Features…** and
**Rust: Select Target…** pick Cargo features (the default ones, all, or some) and a target installed
with rustup. The choice is kept per project and used by the language server and by Build, Run and Test.
JSON (`.json`, `.jsonc`), YAML (`.yaml`, `.yml`) and Markdown (`.md`) files use the JSON language
server (`npm install -g vscode-langservers-extracted`), `yaml-language-server` and `marksman`. The
JSON one only validates: against a file's own local `$schema`, downloading nothing and leaving the
formatting alone; generated files (`compile_commands.json`, `package-lock.json`, `*.min.json`) and
JSON files over 512 KB get no server. YAML files are validated and formatted with the schemas of
the SchemaStore catalog; Markdown files get link and heading navigation and keep word completion.
CMake files (`CMakeLists.txt`, `*.cmake`) use `neocmakelsp`: completion, hover, errors, formatting
(it does not read the found packages' own CMake files, to stay small). Servers are looked for on
`PATH` and in `~/.local/bin`, `~/.cargo/bin` and `~/go/bin`, where pipx, cargo and go install them.
Each server starts on first use and stops 30 seconds after the last file it serves closes. Files are
sent only once their tab is shown, so restored sessions cost the servers little. Other files
(Makefiles, scripts) get completion from the words in the file. "Restart Language
Servers" in the command palette restarts active servers (including one that gave up after crashes);
when the current file's server is not installed, the status bar shows **⚠ No LSP**: a click shows
the commands that install it on your distribution (each with a Copy button), and **Try Again** starts
it once it is there. When clangd cannot find
includes and the project has no `compile_commands.json` or `compile_flags.txt`, the status bar shows
**⚠ No compile flags**; a click explains how to give it the flags (and points out a misnamed
`compile-flags.txt`).

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
[`tasks.md`](tasks.md).

**Status bar**: the git branch (`*` when there are changes), the language servers' errors and
warnings in the open files, **⚠ No compile flags** when clangd lacks them, **⚙ Build** and
**▶ Run** with what they would build (a plugin's or recovery's short messages show there too),
what a language server is busy with (**rust-analyzer: Indexing 45%**, **clangd: starting**),
**⚠ No LSP** when the file's language server is not installed, the file's indentation (**Spaces: 4**, **Tab Size: 8**), the
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
`go.mod`, `package.json`'s `start` / `dev` script, Maven, Gradle, Python from its root with the
picked interpreter or its venv.

**Test.** The command palette's **Test** saves changed files and runs the active task profile's
`test` command, or detects CMake/CTest, Meson, `make test`, Cargo, Go, Node's `test` script, Maven,
Gradle or `pytest` in a Python project. Output stays in the integrated shell; Ctrl-click a
`file:line:column` location to open the failing source.

**Git.** The editor marks changed lines in the gutter (blue changed, green added, red
where lines were deleted), following new commits by itself. **Compare with HEAD** (`Ctrl+Alt+D`)
and **File History** (`Ctrl+Alt+H`, pick a commit) open one tab ("file ↔ commit") with the file
on the left and the old version read-only on the right, scrolling together and lined up (also with
word wrap): changed lines are tinted, their changed words colored (green new, red old), and gap
rows stand for missing lines; it opens at the first change. Only the changes show, with three
lines around each: a row "⋯ N unchanged lines" stands for the rest (click it, or move the cursor
into it, to show them; "Git: Show / Hide Unchanged Lines" toggles). Closing the tab closes both sides; "Git: Stop Comparing" in the
command palette keeps the file, back to comparing with the last commit. The Files panel colors files by their git state (modified,
added, untracked, conflicted, ignored) with the letter at the end of the row. The **Source
Control** panel (`Ctrl+Shift+G`) commits, pushes, stages and unstages changes, and discards after
confirmation. A commit message not committed yet is kept with the project, also when t-code
closes, until the commit; Up on its first line brings back earlier commits' messages (Down goes
back to what was typed). Right-click a modified file to stage or unstage an individual hunk. It also lists
commits: the pointer on one shows its message, author, date and line counts, and "Open Commit Details"
shows it with its diff.

**Branches, pull and fetch.** Click the branch in the status bar (or "Git: Checkout to…") to switch:
the list has the local branches by their last commit, then the remote ones you have no branch of
(picking one makes a local branch tracking it); type a name that is no branch to create it ("Git:
Create Branch…" asks only for the name). Unsaved editors are saved first; when git refuses
(changes in the way), the Source Control panel says why. **Pull** next to Push runs `git pull` as
you set it up (merge or rebase); "Git: Fetch" brings the remote's branches, and Push shows how far
ahead / behind you are.

**Stash.** "Git: Stash…" puts the changes aside (type a message, or none; "Stash, untracked files
too" takes the new files as well). "Git: Pop Stash…", "Git: Apply Stash…" and "Git: Drop Stash…" list
the stashes, newest first: pop brings one back and removes it, apply keeps it, drop removes it after
asking. Unsaved editors are saved first; when a pop conflicts, the stash stays and Source Control
says so.

**Line blame.** Once the cursor rests on a line, who last changed it, when and the commit's subject
show dim after its end ("Ada, 3 days ago · Fix the parser"); a line you changed and did not commit
says "Not committed yet". The "Line blame" setting turns it off.

**Merge conflicts.** In a file git left conflict markers in, the current change (between
`<<<<<<<` and `=======`) is tinted green and the incoming one (up to `>>>>>>>`) blue; after the
`<<<<<<<` line, **Accept Current Change**, **Accept Incoming Change** and **Accept Both Changes**
put that side (or both, current first) in place of the block, as one undo step. The diff3 style's
common ancestor lines (after `|||||||`) are dropped either way. Stage the file in Source Control
once it is resolved.

**Files panel.** A folder is read when you open it and shows new, removed and renamed files by
itself; the git colors follow commits and saves, and an open file with errors or warnings (and
the folders holding it) is named in red or yellow. Files and folders can be cut, copied and pasted
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

**Menu bar.** On the left of the title bar: Files (open a folder, recent folders), T-Code (new
window, command palette, updates, quit), Settings, Panels (each panel, checked while it shows),
About and Help (keyboard shortcuts, reset panel layout). The middle shows the project's name.

**Projects.** For a second project next to the first, middle-click the dock icon (or pick "New Window" from
its menu, or run `t-code --new-window`): a new window opens and asks for a folder.
Switching remembers and closes the current project's tabs, shows the new folder in the Files
panel, restarts the shell there, and brings back that folder's tabs from last time. t-code also remembers
the window size, pane sizes, hidden panes and text size (in `~/.config/t-code/session.ini`).

**Search** (`Ctrl+Shift+F`), a tab next to Files and Source Control: results
come as you type, grouped by file with the matches marked. **Aa**, **ab** and **.\*** next to the
text turn on Match Case, Match Whole Word and regular expressions; **⋯** shows "files to include"
and "files to exclude" (comma separated globs such as `*.c, src`). Click a file to fold its
results, a result to open the file there. ripgrep skips what `.gitignore` ignores; without it,
grep searches everything but `.git` and `build`. **⇄** shows the replacement: each result then
shows its match struck out and what replaces it (with regular expressions, `$1` / `${name}` stand
for a group). **All** (or `Enter` in the field) asks, then replaces every match in the project:
files open in an editor change there (unsaved, `Ctrl+Z` takes it back), the others on disk.

**Color themes.** Dark Classic and other dark and light themes (Gruvbox among them), each with a
preview, in a rounded or a square look; the editor, the terminals and the panels all follow.
`theme.css` and the editor's style scheme can be edited by hand ("Preferences: Open …").

**Settings** (`Ctrl+,` or Settings in the menu bar, its pages listed on the left): minimap, code coloring, indent guides,
word wrap (off by default; with it off, long lines run off the screen and scroll sideways with
the horizontal scrollbar or Shift + wheel), a ruler (a vertical line after a chosen column, like 80
or 120; 80 by default, 0 turns it off), tab size (2 by default), spaces for Tab, detecting a file's own indentation, autosave,
format on save (`Ctrl+S` formats C and C++ first; off by default), clang-tidy checks (the project's
`.clang-tidy`; off by default), checking for updates (the latest release on GitHub against this
one, asking once when a newer one is out; "Check for Updates Now" from the palette asks at any
moment), fonts, text size and reopening the last project. Changes apply at
once, to the open editors too, and are kept in `~/.config/t-code/settings.ini` (a comment over
each key; a hand edit applies at once).

**Log file.** `~/.local/state/t-code/t-code.log` keeps the last 50 starts: a line for each start
(the date, the version and the process id) and for each clean quit (`=== quit`), so a run without
one crashed or was killed, and t-code's and GTK's warnings in between, each with its time. Useful
when t-code was started from the desktop's menu, where no terminal shows them, or when a problem
is noticed later. "Help: Open Log File" opens it.

**Markdown preview** opens as a tab and follows the file as you type. It is drawn natively
(md4c into a GTK text view, no browser engine) and shows what GitHub shows: all of GitHub Flavored
Markdown (headings, lists, task lists, quotes, tables whose cells wrap, strikethrough, autolinks, every
HTML5 entity), code blocks colored like the editor, GitHub's alerts (`> [!NOTE]`, `> [!WARNING]`, ...),
footnotes (`[^1]`), `:emoji:` shortcodes, and the HTML READMEs use: `<b>`, `<i>`, `<ins>`, `<del>`,
`<sub>`, `<sup>`, `<kbd>`, `<mark>`, `<a href>` and `<a name>` anchors, `<img>` with `width`,
`<picture>` (its image for the light or dark theme), `<br>`, `<hr>`, `<h1>` .. `<h6>`, `align="center"`,
and `<details>` that a click on their `<summary>` opens; comments are left out. Images are local files
(a path starting with `/` is from the git repository's root) or web addresses (fetched with `curl`),
SVG included; an image inside a link follows it when clicked, and link titles show over links.

TeX math: `$...$` inline, `$$...$$` and ```` ```math ```` blocks centered (fractions, roots, scripts,
sums and integrals with limits, stretched delimiters, matrices, `cases`, `aligned`), set by t-code
itself with Pango and cairo, no TeX installation needed; in table cells on one line. ```` ```mermaid ````
diagrams are drawn when mermaid-cli (`mmdc`) is installed; without it, and for GeoJSON, TopoJSON and
STL blocks, the code shows under a line saying what it is. Links to other markdown files open their
preview, other local files open in an editor, and web links open in the browser.

## Keys

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
| anywhere    | `Ctrl+=` / `Ctrl+-` / `Ctrl+0`         | bigger / smaller / normal text (in a preview tab: the previews' text only) |
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
| editor      | `Ctrl+/`, `Shift+Alt+↓`, `Ctrl+Shift+K` | comment lines, duplicate, delete them    |
| editor      | `Ctrl+D` / `Ctrl+Shift+L`              | add the next occurrence / all occurrences as cursors |
| editor      | `Ctrl+Shift+↑` / `↓`, `Alt+Click`, `Shift+Alt+drag` | add a cursor above / below, at the click, a column selection |
| editor      | `Alt+↑` / `Alt+↓`                      | move lines up / down                      |
| editor      | `Ctrl+Z` / `Ctrl+Y`, `Ctrl+Shift+Z`    | undo / redo                               |
| editor      | `Ctrl+Space` / `Ctrl+Shift+Space`      | completion / parameter hints              |
| editor      | `F12`, `Ctrl+Click` / `Alt+←`          | go to definition / back                   |
| editor      | `Shift+F12` / `F2`                     | find all references / rename symbol       |
| editor      | `Shift+Alt+H`                          | call hierarchy (the Hierarchy panel)      |
| editor      | hold `Ctrl+Alt`                        | inlay hints the other way while held      |
| editor      | `Shift+Alt+F`                          | format the file with clangd               |
| editor      | `Ctrl+.` / `Ctrl+Shift+.` / `Alt+O`    | quick fix / go to a symbol in the file / switch source and header |
| editor      | `Ctrl+Shift+[` / `Ctrl+Shift+]`        | fold / unfold the block                   |
| editor      | right click                            | quick fix, go to definition, references, symbols, rename, format, comment, breakpoint, watch, compare, history |
| editor      | double click                           | select a name (its other occurrences light up) |
