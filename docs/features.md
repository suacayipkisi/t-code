# Features

What t-code does, in detail. The [README](../README.md) has the short version.

- [The editor](#the-editor)
- [Language servers](#language-servers)
- [Build, run and test](#build-run-and-test)
- [Debugging](#debugging)
- [Version control](#version-control)
- [Files, search and projects](#files-search-and-projects)
- [The window](#the-window)
- [Previews](#previews)
- [Logs and processes](#logs-and-processes)
- [Keys](#keys)

## The editor

Like a plain text editor, without modes: `Ctrl+C` / `Ctrl+V`, `Ctrl+Z`, `Shift`+arrows to select,
the mouse. On top of that:

### Code intelligence

- **clangd's problems** are underlined with a wave (red errors, yellow warnings); resting the
  pointer on them shows the message, and on a symbol its type and signature. The status bar counts
  them, and a strip right of the minimap marks where they are in the whole file (click to go there).
- **Completion while typing** (and after `.`, `->`, `::`, or on `Ctrl+Space`):
  - the list shows each symbol's kind, its parameters and type;
  - `Tab` takes the proposal (`Enter` starts a new line and closes the list);
  - a function goes in with its parameters as placeholders (`Tab` to the next one; with the list
    open over a placeholder, `Tab` takes the proposal first);
  - the `#include` it needs is added by itself.
- **Parameter hints**: in a call's arguments, the signature shows above the cursor with the
  argument being typed in bold.
- **Navigation:**
  - **Go to Definition** (`F12`, `Ctrl+Click`; while Ctrl is held, the name under the pointer is
    underlined) and back (`Alt+←`);
  - **Find All References** (`Shift+F12`), **Rename Symbol** (`F2`, in every file);
  - **Go to Symbol in File** (`Ctrl+Shift+.`), **Go to Symbol in Workspace** (from the command
    palette), **Switch Source / Header** (`Alt+O`);
  - `Ctrl+Click` on a web link (`https://`, `www.`, `mailto:`) opens it in the default browser,
    with or without a language server.
- **Format Document** (`Shift+Alt+F`, with the project's `.clang-format`).
- **Quick fixes** (`Ctrl+.`): clangd's fixes for the problems on the line (a missing `;` or
  `#include`, a misspelled name) and its refactorings, one pick away. Resting the pointer on a
  problem shows its fixes as buttons (`Quick Fix: #include <iostream>`) under the message. Other
  servers' fixes come the same way; pages a server shows (gopls' package documentation and
  assembly) open in the browser.
- **Inlay hints:** parameter names before arguments (`add(left: 1, right: 2)`) and deduced types
  after names (`auto item: Widget = ...`), dim, inside the line; the text itself does not change.
  - From clangd, rust-analyzer, gopls and the TypeScript server, for the shown lines.
  - The "Inlay hints" setting turns them off.
  - Holding `Ctrl+Alt` (alone) turns them the other way while held: shows them when they are off,
    hides them when they are on.
- **Outline and breadcrumbs:**
  - the **Outline** panel ("View: Toggle Outline") lists the current file's classes, functions and
    fields as the language server sees them, the one around the cursor selected; a click goes there;
  - over each editor, the breadcrumbs show the file's folders and the symbols around the cursor
    (`src › shape.cpp › Shape › draw`); a symbol's crumb goes to it. The "Breadcrumbs" setting
    hides them.
- **Call and type hierarchy:** `Shift+Alt+H` (or the editor's right-click menu) on a function shows
  who calls it in the **Hierarchy** panel, as a tree that opens further as you go.
  - **Callers** / **Callees** switch the direction; "Show Type Hierarchy" does the same for a
    type's supertypes and subtypes. A row opens its place.
  - With **Follow** on (the default) and the panel shown, a click on another function shows its
    hierarchy by itself.
  - clangd, gopls and rust-analyzer offer calls; clangd also types.
- **Colors by meaning**: clangd tells functions (yellow), types (teal), macros (blue), constants and
  enum members (bright blue) and variables (light blue) apart; GtkSourceView colors keywords,
  strings, comments and the rest from its theme (`styles/dark-classic.xml`). `.h` files open as
  C++ when they contain C++.
- **Highlights**: the cursor on a name lights up that symbol's uses (clangd's, not other names
  spelled the same); selecting a name lights up its text everywhere. Their lines are marked in the
  strip beside the minimap, as are git's changes. Code an `#if` leaves out is dimmed.

### Editing

- Find and replace (`Ctrl+F`, `Ctrl+H`; case, whole word, regular expressions), go to line
  (`Ctrl+G`).
- Comment lines (`Ctrl+/`), a block comment (`Alt+C`: an empty `/*  */` on a blank line, or around
  the selection or the line; again takes it out), duplicate (`Shift+Alt+↓`), move (`Alt+↑` / `Alt+↓`) and delete
  (`Ctrl+Shift+K`) lines; brackets and quotes that close by themselves (and wrap a selection).
- `Enter` after an opening bracket goes one level deeper (between a pair the closer gets its own
  line), and `}` outdents. A double click selects a name only (letters, digits, `_`).
- **Code folding** (the arrow beside a block, `Ctrl+Shift+[` / `]`) and **sticky scroll**: the
  heads of the function, loop or namespace you are in stay at the top of the editor.
- **Snippets:** type a trigger (`main`, `for`, `if`, `def`, `iferr`, `match`, `af`, ...) and `Tab`;
  `Tab` / `Shift+Tab` go through its fields. The completion list offers them too. t-code brings
  some for C, C++, Python, Go, Rust, JavaScript / TypeScript and shell; your own go in
  `~/.config/t-code/snippets/` ("Preferences: Open User Snippets"), read when t-code starts.
- **Multiple cursors:**
  - `Ctrl+D` selects the name at the cursor, then adds its next occurrence; `Ctrl+Shift+L` selects
    every occurrence;
  - `Ctrl+Shift+↑` / `↓` add a cursor on the line above / below; `Alt+Click` adds (or removes) one;
    `Shift+Alt+drag` selects a column;
  - typing, Backspace, Delete, Enter, Tab, the arrows, Home / End and copy / cut / paste act on all
    of them as one undo step (pasting as many lines as there are cursors gives one line each);
  - `Esc` or a click goes back to one.
- **Right-click menus** on the text, tabs, files, Source Control's changes and commits, search
  results, terminals and the debugger's lists.
- A minimap, indent guides (the block around the cursor in its bracket color), the git diff gutter
  (green added, blue changed, red deleted).

### Saving and crash recovery

- A file with changes is saved a second after the last one (the autosave setting), so `Ctrl+S` is
  rarely needed.
- A tab shows `●` while its file has unsaved changes, and closing it then asks whether to save.
- Unsaved changes are also copied to `~/.cache/t-code/recovery/` a moment after each edit: if t-code
  or the system stops unexpectedly, opening the file again shows a bar that restores them (or
  discards the copy).
- A file changed on disk (a `git checkout`, another program) is loaded again when the editor has
  nothing unsaved.

### Indentation

Each file gets the indentation its project asks for, each step overriding the one before:

1. the Settings' tab size and spaces;
2. what the file itself uses (detect indentation);
3. the project's `.editorconfig` and `.clang-format` (for C and C++);
4. a choice made for the project.

The status bar shows the indentation in use and where it comes from; clicking it (or "Change
Indentation for This Project…" in the command palette) picks one for the whole project, which is
remembered. Makefiles always get tabs.

## Language servers

clangd starts when the first C or C++ file is shown. The other languages use these servers when
the executable is on `PATH`:

| Files | Server |
|-------|--------|
| Python (`.py`) | `pylsp` (also from the project's venv, in a trusted folder) |
| Go (`.go`) | `gopls` |
| Rust (`.rs`) | `rust-analyzer` |
| JavaScript / TypeScript (`.js`, `.jsx`, `.mjs`, `.cjs`, `.ts`, `.tsx`, `.mts`, `.cts`) | `typescript-language-server` (`npm install -g typescript-language-server typescript@6`) |
| JSON (`.json`, `.jsonc`) | the JSON language server (`npm install -g vscode-langservers-extracted`) |
| YAML (`.yaml`, `.yml`) | `yaml-language-server` |
| Markdown (`.md`) | `marksman` |
| CMake (`CMakeLists.txt`, `*.cmake`) | `neocmakelsp` |

- **JSON** only validates: against a file's own local `$schema`, downloading nothing and leaving
  the formatting alone. Generated files (`compile_commands.json`, `package-lock.json`,
  `*.min.json`) and JSON files over 512 KB get no server.
- **YAML** files are validated and formatted with the schemas of the SchemaStore catalog.
- **Markdown** files get link and heading navigation and keep word completion.
- **CMake**: completion, hover, errors, formatting (it does not read the found packages' own CMake
  files, to stay small).
- Other files (Makefiles, scripts) get completion from the words in the file.

**Finding servers.** Servers are looked for on `PATH` and in `~/.local/bin`, `~/.cargo/bin` and
`~/go/bin`, where pipx, cargo and go install them. Started from the desktop's menu, t-code asks the
login shell once for its `PATH` and toolchain variables (`GOPATH`, `CARGO_HOME`, `JAVA_HOME`, ...),
so servers, debuggers and test runners installed through nvm, pyenv, asdf, mise or a toolchain
unpacked in the home folder are found too; folders t-code already had keep their order, and
variables it already had stay.

**Toolchains.** Chosen per project and used by the language server and by Build, Run and Test:

- **Python: Select Interpreter…** (command palette) picks the project's Python among its venvs,
  `$VIRTUAL_ENV`'s and `python3`;
- **Rust: Select Features…** and **Rust: Select Target…** pick Cargo features (the default ones,
  all, or some) and a target installed with rustup.

**Trusted folders.** A cloned project can carry programs of its own, so t-code runs them only in a
folder you trusted: the venv's Python and `pylsp`, `node_modules`' TypeScript, Rust build scripts,
proc macros and the check on save, and the Testing panel listing tests by itself (pytest, cargo and
go test run the project's code). Elsewhere the servers use what is on `PATH` (the TypeScript server
the TypeScript it was installed with) and a bar above the editors offers **Trust Folder** or **Not
Now**. Trusting restarts the servers. The list is `~/.config/t-code/trusted`, one folder per line;
the folders inside a trusted one are trusted too. Build, Run, tasks and the Testing panel's Refresh
are your own request and are not asked about. **Trust all folders** (Settings: General, `trust-all-folders` in
settings.ini) trusts every folder and never asks.

**A server's life.**

- Each server starts on first use and stops 30 seconds after the last file it serves closes
  ("Stop when unused" in Settings: Language Servers, `server-idle-seconds` in settings.ini; -1 keeps
  it running).
- A server that grew while files were closed is restarted once it has been quiet for 30 seconds
  (resident memory 150 MB, or a quarter if that is more, above where it settled after starting; "Restart when
  grown by" in Settings: Language Servers, 0 turns it off); the open files
  go to the new one. Not gopls, rust-analyzer or plugin servers, which would load the whole
  workspace again.
- Files are sent only once their tab is shown, so restored sessions cost the servers little.
- "Restart Language Servers" in the command palette restarts active servers (including one that
  gave up after crashes).
- When the current file's server is not installed, the status bar shows **⚠ No LSP**: a click
  shows the commands that install it on your distribution (each with a Copy button), and **Try
  Again** starts it once it is there.
- When clangd cannot find includes and the project has no `compile_commands.json` or
  `compile_flags.txt`, the status bar shows **No compile flags**; a click explains how to give it
  the flags (and points out a misnamed `compile-flags.txt`).

**Problems panel (`Ctrl+Shift+M`).** The language servers' errors, warnings and notes in every open
file, in one list with their counts on top and `file:line:column` beside each; a click goes there.
It sits in the bottom dock and moves like any other panel.

**Status bar**, from left to right:

- the git branch (`*` when there are changes);
- the language servers' errors and warnings in the open files;
- **No compile flags** when clangd lacks them, **No LSP** when the file's language server is not
  installed;
- **Build** and **Run** with what they would build (a plugin's or recovery's short messages show
  there too);
- what a language server is busy with (**rust-analyzer: Indexing 45%**, **clangd: starting**);
- the file's indentation (**Spaces: 4**, **Tab Size: 8**), the cursor's line and column, and
  **Keybinds**.

## Build, run and test

### Build and run

`Ctrl+F5` / Run saves, then builds and runs in the shell (Build stops after building), looking
upwards from the current file:

- CMake uses `CMakeLists.txt`, Meson uses `meson.build`, and Make uses a Makefile;
- otherwise the current C or C++ file is compiled on its own (with AddressSanitizer and UBSan) into
  `build/` and run;
- a script runs by its interpreter: Python, JavaScript / TypeScript with node, shell, Lua, Ruby,
  Perl, PHP, Go, Java, or any file with `#!`;
- projects run by their files: Cargo, `go.mod`, `package.json`'s `start` / `dev` script, Maven,
  Gradle, Python from its root with the picked interpreter or its venv.

Ctrl+click a `file.c:12:5` in compiler output to go there.

### Project tasks

A `.t-code/tasks.json` (or `t-code.json`) in the project tells Build, Run, Test and F5 what to run
(with arguments, environment, working folder, and named profiles such as `debug` / `release`),
instead of t-code guessing; the commands run in the shell. See [`tasks.md`](tasks.md).

### Test

The command palette's **Test** saves changed files and runs the active task profile's `test`
command, or detects CMake/CTest, Meson, `make test`, Cargo, Go, Node's `test` script, Maven, Gradle
or `pytest` in a Python project. Output stays in the integrated shell; Ctrl-click a
`file:line:column` location to open the failing source.

### Testing panel

The **Testing** panel ("View: Toggle Testing") lists the project's tests as a tree:

- **CTest's**, from a CMake build folder (`build/` or another one in the project folder).
  GoogleTest cases come under their suite, whether through `gtest_discover_tests` or from a
  GoogleTest program that CTest runs as one test: its cases are listed with `--gtest_list_tests`
  and run through CTest with GoogleTest's filter.
- **pytest's**, under their file, with the project's Python.
- **`go test`'s**, under their package; **`cargo test`'s**, under their module.
- **JUnit's**, in a Maven or Gradle project: JUnit 4 and 5 test methods under their class, found in
  the Java files; run through `mvn test -Dtest=…` or `gradle test --tests …` (`mvnw` / `gradlew`
  when the project has them), their XML reports read.

**Results and running.**

- Each test shows its last result: ✓ passed, ✗ failed, - skipped, ○ not run.
- ▶ on a row (shown on hover) runs that test or group; the header has Run All, Run Failed, Stop and
  Refresh (also "Test: Run All Tests", "Test: Run Failed Tests", "Test: Refresh Tests" and "Test:
  Stop" in the command palette).
- Runs save the changed files first and go on in the background, one at a time; CTest builds before
  it runs.
- A selected test's output (and why it failed) shows under the tree; a group shows its framework's
  last command output.
- Tests are listed only when asked for: when the panel first shows, on Refresh, and after a run (go
  and cargo compile to list). The list stays while the project is open.

**Debugging a test.** A CTest test has a debug button (shown on hover): the project is built, then
gdb runs the test's command (a GoogleTest case with its `--gtest_filter`) in the folder CTest
would, stopping at your breakpoints; Restart builds and debugs that test again. A JUnit test's
debug button runs it with its JVM waiting, and the Java plugin attaches java-debug to it; its
result shows as a Run's would.

**In the editor.**

- A double click on a test opens its source at the line that defines it (`TEST(Suite, Name)`,
  `def test_…`, `func Test…`, `fn …`, a JUnit method; a CTest test without one opens its `add_test`
  line).
- Once the tests are listed, an open file shows ▶ in the gutter on each line that defines one (and
  on a `CMakeLists.txt`'s `add_test` lines), colored by its last result.
- A click on it runs that test (a pytest class: its tests); a right click offers Run, Debug (a
  CTest test) and Show in Testing.

## Debugging

`F5` saves, builds with debug info and starts the program under a debugger:

| Language | Debugger |
|----------|----------|
| C, C++, Rust (Cargo or a file alone) | gdb 14 or newer (`gdb -i dap`); lldb-dap instead with "Debug with lldb-dap" in Settings → C / C++ |
| Python | debugpy (`python3 -m pip install debugpy`, the project's interpreter), no build |
| Go | Delve (`go install github.com/go-delve/delve/cmd/dlv@latest`), no build |
| Java (plugin) | java-debug inside jdtls: the Java plugin's `F5` debugs the main class |

Each says how to install it when it is missing. A Makefile's program is picked once and remembered.

**Breakpoints.** A click left of a line number (or `F9`) puts a breakpoint there; a right click
asks for a condition or a log message. A logpoint (a diamond) prints `i = {i}` with the value in
the Debug panel each time the line runs, and the program goes on.

**When it stops**, the line is marked, and:

- the Run and Debug panel shows the variables (the ones that changed in color, arrays and structs
  opened like folders), watch expressions, the call stack and every breakpoint;
- the pointer on a name shows its value;
- the Debug panel at the bottom has the buttons (continue, pause, step over / into / out, restart,
  stop), the program's own terminal and a line to evaluate expressions (or `-exec` gdb commands);
- a crash (segfault, AddressSanitizer) stops on the bad line and says in one sentence what
  happened.

**Low-level views.**

- **ASM** opens the Disassembly View: the function's instructions with its source lines, stepping
  by instruction and instruction breakpoints (gdb).
- **MEM** opens the Memory View: the bytes a pointer, an array or a variable points to (`p`, `arr`,
  `&n`, `0x7ffd1230`), eight a line as hex and as characters, read again at each pause with the
  bytes that changed listed under them.

**Attaching and core dumps.**

- "Debug: Attach to Process…" lists your running programs (newest first) and attaches to the one
  picked: it stops where it is, and Stop detaches, leaving it running (a Python one goes to
  debugpy). Where the system allows a debugger only its own children (`kernel.yama.ptrace_scope`
  1), the Debug panel says how to allow it.
- "Debug: Open Core Dump…" lists the crashes systemd-coredump kept (or opens a core file, then asks
  for its program) and shows where it crashed, with the stack and the variables as they were;
  nothing runs.

## Version control

### Git in the editor

- **Diff gutter:** changed lines are marked in the gutter (blue changed, green added, red where
  lines were deleted), following new commits by itself.
- **Line blame:** once the cursor rests on a line, who last changed it, when and the commit's
  subject show dim after its end ("Ada, 3 days ago · Fix the parser"); a line you changed and did
  not commit says "Not committed yet". The "Line blame" setting turns it off.
- **Files panel:** files are colored by their git state (modified, added, untracked, conflicted,
  ignored) with the letter at the end of the row.

### Comparing

**Compare with HEAD** (`Ctrl+Alt+D`) and **File History** (`Ctrl+Alt+H`, pick a commit) open one
tab ("file ↔ commit") with the file on the left and the old version read-only on the right:

- both sides scroll together and are lined up (also with word wrap);
- changed lines are tinted, their changed words colored (green new, red old), and gap rows stand
  for missing lines; it opens at the first change;
- only the changes show, with three lines around each: a row "⋯ N unchanged lines" stands for the
  rest (click it, or move the cursor into it, to show them; "Git: Show / Hide Unchanged Lines"
  toggles);
- closing the tab closes both sides; "Git: Stop Comparing" in the command palette keeps the file,
  back to comparing with the last commit.

### Source Control panel

The **Source Control** panel (`Ctrl+Shift+G`):

- commits, pushes, stages and unstages changes, and discards after confirmation;
- right-click a modified file to stage or unstage an individual hunk;
- a commit message not committed yet is kept with the project, also when t-code closes, until the
  commit; Up on its first line brings back earlier commits' messages (Down goes back to what was
  typed);
- it also lists commits: the pointer on one shows its message, author, date and line counts, and
  "Open Commit Details" shows it with its diff.
- a click on a commit lists its files; a click on a file compares it with its parent commit's version
  in a temporary tab (in italics) that the next file replaces, so reading the history leaves no pile
  of tabs; a double or middle click keeps it (as does a double click on the tab).

### Branches, pull and fetch

- Click the branch in the status bar (or "Git: Checkout to…") to switch: the list has the local
  branches by their last commit, then the remote ones you have no branch of (picking one makes a
  local branch tracking it).
- Type a name that is no branch to create it ("Git: Create Branch…" asks only for the name).
- Unsaved editors are saved first; when git refuses (changes in the way), the Source Control panel
  says why.
- **Pull** next to Push runs `git pull` as you set it up (merge or rebase); "Git: Fetch" brings the
  remote's branches, and Push shows how far ahead / behind you are.

### Stash

- "Git: Stash…" puts the changes aside (type a message, or none; "Stash, untracked files too" takes
  the new files as well).
- "Git: Pop Stash…", "Git: Apply Stash…" and "Git: Drop Stash…" list the stashes, newest first: pop
  brings one back and removes it, apply keeps it, drop removes it after asking.
- Unsaved editors are saved first; when a pop conflicts, the stash stays and Source Control says
  so.

### Merge conflicts

In a file git left conflict markers in, the current change (between `<<<<<<<` and `=======`) is
tinted green and the incoming one (up to `>>>>>>>`) blue. After the `<<<<<<<` line, **Accept
Current Change**, **Accept Incoming Change** and **Accept Both Changes** put that side (or both,
current first) in place of the block, as one undo step. The diff3 style's common ancestor lines
(after `|||||||`) are dropped either way. Stage the file in Source Control once it is resolved.

### Several repositories

When the project holds other git repositories (clones in its folders, or a folder that is no
repository with repositories in it; also ones your `.gitignore` leaves out), Source Control lists
them under **Repositories** with their branch and number of changes.

- Click one to commit, stage, push or switch branches in it (the status bar's branch follows).
- Submodules are not listed: they belong to the repository that holds them.
- Line blame and the diff gutter use each file's own repository.

### Mercurial and Subversion

A Mercurial repository or a Subversion working copy works like a git one (`hg` or `svn` on your
`PATH`): the Files panel's colors, the diff gutter, line blame (as saved: nothing shows while the
file has unsaved edits), Compare with HEAD, File History, and Source Control with its changes,
Commits and a commit's files.

**Staging.** Neither has git's staging area: **+** puts a file in Staged Changes (a new file is
added with `hg add` / `svn add`), and Commit takes the staged files, or every change when none are
staged.

**Pull and push.** Mercurial has Pull (`hg pull --update`) and Push; for Subversion the Pull button
is **Update** (`svn update`) and there is no Push (a commit goes to the server).

**Conflicts** (an `hg merge`, an `svn update`) show with **!** and the editor's conflict buttons;
once the file is fixed, **+** marks it resolved (`hg resolve --mark`, `svn resolve --accept
working`), and during an hg merge Commit takes the whole merge.

**Branches.**

- Mercurial lists its bookmarks and open named branches (`hg update`; a new branch is a bookmark).
- Subversion lists `trunk` and the folders in `branches/` (`svn switch`; a new branch is copied on
  the server from the one checked out, then switched to).

**Stash and changelists.**

- In Mercurial the stash commands use `hg shelve` and `hg unshelve` (a shelve is picked by its
  name; Apply keeps it).
- Subversion's changelists show on their files' rows, and "Subversion: Changelists…" stages a
  changelist's files (Commit then takes just them), puts the staged files in a changelist (type its
  name) or takes them out.
- Subversion's stash and staging hunks in either are git's only for now.

**Mixed repositories.**

- When several repositories hold the project folder (an svn checkout inside a git repository, or
  hg-git's `.git` next to `.hg`), Repositories lists each with its kind and the nearest is used;
  click another to make it the project's own (the Files panel and the gutters follow it, and the
  project remembers it).
- Mercurial repositories and Subversion working copies inside a git project show in Repositories
  too (also in a folder .gitignore leaves out), and their files are not counted as the git
  project's new files.

**New repositories.** In a folder that is no repository, Source Control offers **Initialize
Repository** with Git, Mercurial or Subversion to choose. Subversion keeps its repository apart from
the working copy: type the server's URL to check the folder out from it, or leave it empty for a
repository of its own on this computer (made in `~/.local/share/t-code/svn/<folder name>` with a
`trunk`). Either way the folder's files stay, listed as new until you stage and commit them.

## Files, search and projects

### Files panel

- A folder is read when you open it and shows new, removed and renamed files by itself; the git
  colors follow commits and saves, and an open file with errors or warnings (and the folders
  holding it) is named in red or yellow.
- Files and folders can be cut, copied and pasted (a name that is taken gets " copy"), dragged onto
  a folder (from another file manager they are copied), or dragged onto the editors to open them.
- A new file opens in the editor.
- A click opens a file in a temporary tab (its name in italics) that the next click replaces, so
  browsing does not pile up tabs; editing the file or double-clicking it (in the tree or on the
  tab) keeps it, and a middle click opens it to the side.

### File templates

- "Save as Template…" on a tab's right-click menu (or "Template: Save File as Template…") asks for a
  name and a short description and keeps the file's text, unsaved changes too, in
  `~/.config/t-code/templates/` (a template with the same name is replaced).
- Making a new file with the same extension (or the same name, like `Makefile`) then offers "Empty
  File" and those templates.
- "New File from Template…" in the Files panel's right-click menu (or "Template: New File from
  Template…") picks one first and asks the name after, the template's file name filled in.
- "Template: Delete Template…" removes one.

### Search

**Search** (`Ctrl+Shift+F`), a tab next to Files and Source Control: results come as you type,
grouped by file with the matches marked.

- **Aa**, **ab** and **.\*** next to the text turn on Match Case, Match Whole Word and regular
  expressions; **⋯** shows "files to include" and "files to exclude" (comma separated globs such as
  `*.c, src`).
- Click a file to fold its results, a result to open the file there.
- ripgrep skips what `.gitignore` ignores; without it, grep searches everything but `.git` and
  `build`.
- **⇄** shows the replacement: each result then shows its match struck out and what replaces it
  (with regular expressions, `$1` / `${name}` stand for a group).
- **All** (or `Enter` in the field) asks, then replaces every match in the project: files open in an
  editor change there (unsaved, `Ctrl+Z` takes it back), the others on disk.

### Projects

- For a second project next to the first, middle-click the dock icon (or pick "New Window" from its
  menu, or run `t-code --new-window`): a new window opens and asks for a folder.
- Switching remembers and closes the current project's tabs, shows the new folder in the Files
  panel, restarts the shell there, and brings back that folder's tabs from last time.
- t-code also remembers the window size, pane sizes, hidden panes and text size (in
  `~/.config/t-code/session.ini`).

## The window

### Split editor

The editor area holds any number of tab groups, side by side and one over another (two on the left,
three stacked on the right, ...).

- `Ctrl+\` puts a new group right of the one you are in; pressed in that new, still empty group, it
  closes it again.
- "Split Editor Down", "Close Editor Group" and "Join All Editor Groups" are in the command palette,
  "Split Right" / "Split Down" in a tab's menu.
- Drag a tab and blue markers show where it can go: the middle square of the group under the
  pointer puts it into that group, the four around it give it a new group on that side, and the
  four along the edges of the whole area a new group along that edge; the space it would take is
  shaded first. Dropped on a tab strip, it lands where the line shows.
- New files open in the group you are in; "Open to the Side" opens a file in the next group (making
  one when there is none).
- A group closes when its last tab does, `Ctrl+1` … `Ctrl+9` go to a group, and the layout comes
  back with the project.
- A file is never open in two groups at once, since two editors saving the same file would
  overwrite each other.

**Second window.** "Open in New Window" (the Files panel's menu, a tab's menu) moves a file into a
second window of the same project, sharing its language servers; it splits into groups the same
way, and closing it moves its tabs back.

### Menu bar

On the left of the title bar: Files (open a folder, recent folders), T-Code (new window, command
palette, updates, quit), Settings, Panels (each panel, checked while it shows), About and Help
(keyboard shortcuts, reset panel layout). The middle shows the project's name.

### Panels

Files, Source Control, Search and Run and Debug start on the left; the Terminal, Debug and Problems
at the bottom; Outline, Hierarchy and Testing on the right. Each side holds any number of docks,
split either way.

- **Moving:** drag a panel's icon and blue markers show where it can go:
  - over a dock, its middle square adds the panel to that dock's tabs and the four around it give
    it a new dock on that side;
  - over the dock's tab bar a line shows where among the tabs it lands;
  - over the editor area, squares along its left, bottom and right edges put it into a new dock at
    the end of that side.
- A dock goes with its last panel, and the layout comes back next time.
- Plugins can add panels of their own (views, docs/plugins.md).
- Panels get narrower as their side does (shorter buttons; icons that no longer fit go into the
  dock's `⋯` menu).
- A side dragged smaller than it can show closes when you let go; drag it back out before letting
  go and it stays. To open a hidden side, drag from that edge of the window, the way you would
  resize it.

**A cleaner window.** The **Panels** menu (also Settings → Interface) turns off what you do not
use.

- A panel turned off opens nowhere: its icon goes, its shortcut does nothing, and it comes back
  where it was when you turn it on again.
- Source Control turned off also stops git in the background (no branch, file colors, diff marks or
  line blame).
- Under **Window** the same menu hides the menu bar (t-code's icon then opens the menus), the
  status bar, its Build and Run buttons, the title bar's sidebar buttons and its search bar.
- Right-click the title bar's empty part for the same choices (instead of the window manager's
  menu), or a dock's tab bar beside its icons for them plus collapsing or hiding that dock.
- Right-click the status bar to choose each of its items: the branch, errors and warnings, Build
  and Run, the line and column, every plugin's item one by one, and the rest.

### Terminals

The Terminal panel holds the shells as tabs.

- `+` (or `Ctrl+Shift+T`, ``Ctrl+Shift+` ``) opens another one, its `×` or `exit` closes it, a
  double click on a tab renames it ("Terminal: Rename Terminal…" too).
- The first tab is always there and Build, Run and Test type into it; "Open in Terminal" and the
  `terminal` socket command go to the tab showing.
- Each project keeps its other tabs, with their folders and names, for the next time.

### Color themes

- Dark Classic and other dark and light themes (Gruvbox among them), each with a preview, in a
  rounded or a square look; the editor, the terminals and the panels all follow.
- The rounded look's corner radius is a setting (Settings, Appearance: 0 to 24 px; buttons, tabs
  and menus follow).
- The editors' selection is the theme's accent color over the text; how strongly it shows is
  another (Selection opacity, 0 to 100 %, 40 by default).
- `theme.css` and the editor's style scheme can be edited by hand ("Preferences: Open …"); every color and part
  `theme.css` can change, with examples: [theming.md](theming.md).
- Plugins can bring more: each GtkSourceView style scheme in a plugin's `styles` folder is a theme
  (docs/plugins.md).

### Settings

**Settings** (`Ctrl+,` or Settings in the menu bar, its pages listed on the left):

- **General:** autosave, reopening the last project, checking for updates (the latest release on
  GitHub against this one, asking once when a newer one is out; "Check for Updates Now" from the
  palette asks at any moment).
- **Editor:** minimap, sticky scroll, code coloring, indent guides, word wrap (off by default; with
  it off, long lines run off the screen and scroll sideways with the horizontal scrollbar or Shift +
  wheel), inlay hints, breadcrumbs, line blame, a ruler (a vertical line after a chosen column,
  like 80 or 120; 80 by default, 0 turns it off).
- **Indentation:** tab size (2 by default), spaces for Tab, detecting a file's own indentation.
- **Appearance:** theme and look, corner radius, selection opacity, fonts, text size.
- **Interface:** the parts of the window and the panels to turn off (see
  [A cleaner window](#panels)).
- **C / C++:** format on save (`Ctrl+S` formats C and C++ first; off by default), clang-tidy checks
  (the project's `.clang-tidy`; off by default), debugging with lldb-dap.
- **Language Servers:** how long a server keeps running after its last file closes (30 seconds by
  default, -1: until t-code quits); each built-in one on or off (off: its files go to a plugin's
  server for them, or to none).
- **Keyboard Shortcuts:** every command with its keys, and a filter. Click a command's keys and
  press new ones (`Escape` keeps the old ones, `Backspace` removes them); a command that had the
  same keys loses them, and the arrow beside gives the default back. They are kept in
  `~/.config/t-code/keybindings.ini`.

Changes apply at once, to the open editors too, and are kept in `~/.config/t-code/settings.ini` (a
comment over each key; a hand edit applies at once).

## Previews

### Markdown

The **Markdown preview** opens as a tab and follows the file as you type. It is drawn natively
(md4c into a GTK text view, no browser engine) and shows what GitHub shows:

- all of GitHub Flavored Markdown: headings, lists, task lists, quotes, tables whose cells wrap,
  strikethrough, autolinks, every HTML5 entity;
- code blocks colored like the editor;
- GitHub's alerts (`> [!NOTE]`, `> [!WARNING]`, ...), footnotes (`[^1]`), `:emoji:` shortcodes;
- the HTML READMEs use: `<b>`, `<i>`, `<ins>`, `<del>`, `<sub>`, `<sup>`, `<kbd>`, `<mark>`,
  `<a href>` and `<a name>` anchors, `<img>` with `width`, `<picture>` (its image for the light or
  dark theme), `<br>`, `<hr>`, `<h1>` .. `<h6>`, `align="center"`, and `<details>` that a click on
  their `<summary>` opens; comments are left out.

**Images** are local files (a path starting with `/` is from the git repository's root) or web
addresses (fetched with `curl`, four at a time, https or http only; **Web images in previews** in
Settings, `preview-web-images` in settings.ini, turns fetching off), SVG included (one bigger than
16 million pixels drawn is not shown); an image inside a link follows it when clicked, and link titles
show over links.

**Links** to other markdown files open their preview, other local files open in an editor, and web
and mail links open in the browser or mail app; links of other kinds are not followed.

**TeX math:** `$...$` inline, `$$...$$` and ```` ```math ```` blocks centered (fractions, roots,
scripts, sums and integrals with limits, stretched delimiters, matrices, `cases`, `aligned`), set by
t-code itself with Pango and cairo, no TeX installation needed; in table cells on one line.

**Diagrams:** ```` ```mermaid ```` diagrams are drawn when mermaid-cli (`mmdc`) is installed;
without it, and for GeoJSON, TopoJSON and STL blocks, the code shows under a line saying what it is.

### Images and other files

- **Images** (`.png`, `.jpg`, `.gif`, `.webp`, `.bmp`, `.ico`, `.tiff`, `.avif`) open in a tab that
  shows them, fitted to it (a click shows one at its own size), with its size and kind under it.
- An SVG opens in an editor, and its preview (`Ctrl+Shift+V`) draws it.
- **PDFs**, office documents, archives, audio, video and fonts open in the desktop's app for them
  instead of as text.

## Logs and processes

### Log file

`~/.local/state/t-code/t-code.log` keeps the last 50 starts. "Help: Open Log File" opens it.

- A line for each start (the date, the version and the process id) and for each clean quit
  (`=== quit`), so a run without one crashed or was killed.
- t-code's and GTK's warnings in between, each with its time.
- Notes say which language server and debugger started from which file and what the login shell
  added to `PATH`.
- A server or debugger that did not start or stopped by itself is a warning with the reason and the
  last lines it wrote to stderr (a server that does not answer within two minutes counts too).
- A server shown as "No LSP" is logged once with the `PATH` it was looked for on.

Useful when t-code was started from the desktop's menu, where no terminal shows them, or when a
problem is noticed later.

### Process Monitor

"Help: Process Monitor" (also T-Code → Process Monitor…, and Process Monitor in the Plugins window's
⋯ menu) shows t-code and every program it runs, as a tree: plugins' programs, language servers,
gdb, each terminal tab's shell and what runs in it, and short-lived helpers (git, rg).

- Each has its **CPU** (percent of one core), **memory** (PSS: a shared library counts once, split
  between its users), **RSS** (shared pages counted in full for each process, as most system
  monitors show it, so it is bigger and its sum is more than is really used) and **threads**.
- The top totals them by owner, the biggest first, so a slow machine can be told apart: t-code
  itself, or one of its programs.
- **End** asks a process to quit (SIGTERM; a plugin's program stops the way Stop does).
- It looks again every 2 seconds while it is open.

## Keys

The **Keybinds** button in the status bar shows all of them, `Ctrl+Shift+P` (command
palette) finds any command by name, and **Settings → Keyboard Shortcuts** changes them.

**The search bar** in the middle of the title bar is where `Ctrl+P` types: click it (or press the
key) and the results drop down from the bar.

- Type part of a file's name to open it, or start with `#` to find a command instead, t-code's own
  and every plugin's.
- `Ctrl+Shift+P` opens it with `# ` already typed; delete the `#` and it is back to files.
- Turn the bar off under **Panels → Window** or Settings → Interface: both keys keep working, in a
  box over the top of the editors.

| Where       | Key                                    | Action                                    |
|-------------|----------------------------------------|-------------------------------------------|
| anywhere    | `Ctrl+P`                               | open a file by typing part of its name (`#` first: a command) |
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
| anywhere    | `Ctrl+\`                               | a new editor group on the right / close an empty one |
| anywhere    | `Ctrl+1` … `Ctrl+9`                    | editor group 1 … 9                        |
| anywhere    | `Ctrl+PageDown` / `Ctrl+PageUp`, `Alt+.` / `Alt+,` | next / previous tab           |
| anywhere    | `Alt+1` / `Alt+2` / `Alt+3`            | Files / editor / shell                    |
| anywhere    | `Ctrl+B` / `Ctrl+J`                    | hide / show the Files panel / the shell   |
| anywhere    | `Ctrl+Shift+T`, ``Ctrl+Shift+` ``      | a new terminal tab                        |
| anywhere    | `F7`                                   | code coloring on / off (all tabs)         |
| anywhere    | `Ctrl+Alt+L`                           | relative line numbers on / off            |
| anywhere    | `Ctrl+=` / `Ctrl+-` / `Ctrl+0`         | bigger / smaller / normal text (in a preview tab: the previews' text only) |
| shell       | `Ctrl+Click` on `file.c:12:5`          | open the file at that line (compiler messages) |
| tab         | left click / middle click / `×`        | switch to / close / close                 |
| terminal tab | double click / `×`                    | rename / close                            |
| tab         | double click                           | keep a temporary (italic) tab             |
| files       | click, `Enter`, `l`                    | open or close a folder, or open the file in a temporary tab (italic; the next click replaces it) |
| files       | double click / middle click            | open the file for good / to the side      |
| files       | `→` / `←`, `h`, `Backspace`            | open a folder or go into it / close it or go to its parent |
| files       | `↑` `↓`, `j` `k`, `g` `G`, wheel        | move / first / last / scroll              |
| files       | `a` / `r`, `F2` / `d`, `Delete`        | new file (`a/b.c` makes folders, end with `/` for a folder) / rename / delete |
| files       | `Ctrl+X` / `Ctrl+C` / `Ctrl+V`         | cut / copy / paste files and folders      |
| files       | drag a row onto a folder / the editors | move it there / open it                   |
| files       | `.` / `R`                              | hide / show dotfiles (shown by default) / read the tree again |
| files       | right click                            | Open, Open to the Side, Open Preview, Compare with HEAD, File History, New File/Folder, New File from Template, Open in Terminal, Cut/Copy/Paste, Copy (Relative) Path, Rename, Delete, Open Folder as Project |
| editor      | `Ctrl+S` / `Ctrl+W`                    | save / close this tab                     |
| editor      | `Ctrl+F` / `Ctrl+H`, `F3` / `Shift+F3` | find / replace, next / previous           |
| editor      | `Ctrl+/`, `Shift+Alt+↓`, `Ctrl+Shift+K` | comment lines, duplicate, delete them    |
| editor      | `Alt+C`                                | block comment (`/*  */` on a blank line)  |
| editor      | `Ctrl+D` / `Ctrl+Shift+L`              | add the next occurrence / all occurrences as cursors |
| editor      | `Ctrl+Shift+↑` / `↓`, `Alt+Click`, `Shift+Alt+drag` | add a cursor above / below, at the click, a column selection |
| editor      | `Alt+↑` / `Alt+↓`                      | move lines up / down                      |
| editor      | `Ctrl+Z` / `Ctrl+Y`, `Ctrl+Shift+Z`    | undo / redo                               |
| editor      | `Ctrl+Space` / `Ctrl+Shift+Space`      | completion / parameter hints              |
| editor      | `F12`, `Ctrl+Click` / `Alt+←`          | go to definition / back                   |
| editor      | `Ctrl+Click` on a web link             | open it in the default browser            |
| editor      | `Shift+F12` / `F2`                     | find all references / rename symbol       |
| editor      | `Shift+Alt+H`                          | call hierarchy (the Hierarchy panel)      |
| editor      | hold `Ctrl+Alt`                        | inlay hints the other way while held      |
| editor      | `Shift+Alt+F`                          | format the file with clangd               |
| editor      | `Ctrl+.` / `Ctrl+Shift+.` / `Alt+O`    | quick fix / go to a symbol in the file / switch source and header |
| editor      | `Ctrl+Shift+[` / `Ctrl+Shift+]`        | fold / unfold the block                   |
| editor      | right click                            | quick fix, go to definition, references, symbols, rename, format, comment, breakpoint, watch, compare, history |
| editor      | double click                           | select a name (its other occurrences light up) |
