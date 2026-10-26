# Contributing to T-Code

Thank you for your interest in contributing to **T-Code**!

T-Code is designed to be a lightweight, ultra-fast, and native C/C++ IDE for Linux built with GTK4 and C23. To keep the project clean, fast, and maintainable, please follow these guidelines when contributing.

---

## 1. Pull Request Guidelines

### Keep PRs Small and Focused
* **One thing per PR:** Please avoid bundling multiple unrelated features, packaging changes, and bug fixes into a single massive PR.
* Smaller, atomic PRs are easier to review, test, and merge safely.
* If you want to propose a large architectural change, open an Issue first to discuss the design.

### Stay Rebased on `main`
* Always create your branch from the latest `main`:
  ```bash
  git checkout main
  git pull origin main
  git checkout -b feature/your-feature-name
  ```
* Before submitting your pull request, rebase your branch on top of `main` to avoid merge conflicts:
  ```bash
  git fetch origin
  git rebase origin/main
  ```

### Clear Commit Messages
* Write clear, descriptive commit messages summarizing what was changed and why (e.g., `feat(plugins): support custom shortcuts`, `fix(lsp): cancel debounce on tab close`).

---

## 2. Development & Building

### Dependencies
Building T-Code requires a C23 compiler (GCC 14+ or Clang 18+) and standard development libraries:
* `gtk4`, `gtksourceview-5`, `vte-2.91-gtk4`, `json-glib-1.0`, `md4c`
* Build tools: `make`, `pkg-config`, `git`

### Compile
T-Code uses incremental compilation with object files:
```bash
make -j$(nproc)
```
Run the built executable:
```bash
./build/t-code
```

---

## 3. Testing & CI Verification

Before submitting your PR, verify that all tests pass locally:

### Run the Test Suite
```bash
make test
```

### Memory & Sanitizer Checks
T-Code is leak-strict. Run AddressSanitizer and UndefinedBehaviorSanitizer checks:
```bash
make test-asan
```

### Code Formatting
Ensure all C source and header files comply with `clang-format`:
```bash
clang-format --dry-run --Werror $(git ls-files 'src/*.c' 'src/*.h')
```
To automatically format modified files:
```bash
clang-format -i $(git ls-files 'src/*.c' 'src/*.h')
```

### UI & Headless Test Considerations
* Tests run in continuous integration inside headless containers using `Xvfb` and GTK's `Broadway` backend.
* When writing or modifying UI tests, do not rely on fixed physical monitor dimensions or unallocated widget geometries. Ensure layout and main loop iterations (`run(...)`) complete before sampling coordinates.

---

## 4. Code Conventions

* **English only** in code: comments, identifiers, log messages, commit messages.
* **Naming:** types and enums `PascalCase`, enum constants `UPPER_SNAKE_CASE`, functions, variables
  and struct fields `snake_case`; GObject types are `Tcode<Name>`.
* **Modules:** each is `src/<area>/<name>.c` (a big one a folder, `src/<area>/<name>/`) with a
  header documenting what other modules may call; one header may serve several `.c` files, and a
  folder's internal header (`state.h`, `docks.h`, `panel.h`, `runner.h`) is included only by its
  own files. Everything else is `static`. A `.c` includes its own header first, others by their
  path under `src/`.
* **GLib-only modules stay GLib-only** (`editor/text/{detect,indentconf,blocks}.c`,
  `git/linediff.c`, `git/patch.c`, `search/searchmatch.c`, `lsp/compiledb.c`, `lsp/json.c`,
  `debug/dap.c`, `project/langenv.c`, `files/{fileops,folder,gitmarks,icons}.c`): they are tested
  without a display and under ASan.
* **Low memory is the point of t-code:** long-lived code frees everything per tab and per
  document, and async answers that can arrive after a tab is gone must not touch it (cancellables,
  tokens, or looking the tab up again). Measure with `tools/` instead of guessing.
* **Add a test** with changes to the editor, the language server client, the debugger, search,
  the Files panel, the git gutter, plugins or the socket.
* **Keep the docs in step:** a new socket command goes into `src/ipc/socket.h` and
  `docs/plugins.md`; a new key into `src/window/commands.c` (the palette and the Keybinds window
  follow) and the key table in `docs/features.md`.

---

## 5. Packaging Guidelines

T-Code packages are distributed in multiple formats (Native packages for Fedora/Arch/Debian/Void, AppImage, Flatpak, Snap, AUR).
* **Subprocess Safety:** T-Code spawns developer tools (terminal shells, `clangd`, `gcc`, `make`, `gdb`, `git`). Packaging scripts must **never** export a blanket `LD_LIBRARY_PATH` that could poison or break system compilers in the embedded terminal. RPATH (`$ORIGIN`) isolation must be preserved.
* Test packaging changes using `packaging/build.sh <format>` and `packaging/test.sh <format>`.

---

Thank you for helping make T-Code better!

## 6. Source Layout

```
src/                 t-code (src/app.h: struct App and struct Tab, the shared state)
  main.c             only main()
  application/       the GTK application and signals (application.c), the command line (cli.c)
  window/            building and closing the window (window.c), the menu bar and title bar
                     (titlebar.c), the docks (docks/), terminals, the status bar, every command:
                     shortcuts, palette and Keybinds list (commands.c), theme, palette colors
  editor/            tabs and the two groups (tabs.c, tabmenu.c), opening, saving, autosave
                     (editor.c), the page with the minimap (editorview.c), crash recovery;
                     gutter/ (diff gutter, breakpoints, folding), decor/ (indent guides, sticky
                     scroll, breadcrumbs, occurrences, line blame, merge conflicts), editing/
                     (editing commands, multiple cursors, snippets, the find bar), text/ (GLib-only:
                     a file's indentation and language, .editorconfig / .clang-format, blocks),
                     preview/ (the Markdown preview)
  lsp/               the language servers: the client (client/), which file gets which server
                     (servers.c), the channel, the Problems panel, the install helper; inline/
                     (diagnostics, hover, completion, signature, inlay hints, semantic colors),
                     symbols/ (symbols, Outline, call / type hierarchy, navigation, edits)
  debug/             the DAP client (dap.c), the session and Debug panel (debugger.c), the Run and
                     Debug panel (view/), debug hover, disassembly
  git/               the Source Control panel (scm/: status, staging, commit, branches, stash,
                     Commits), git status in the background (gitstatus.c), comparisons and history
                     (compare.c), line differences (linediff.c), per-hunk patches (patch.c)
  files/             the Files panel (files.c, rows.c, actions.c), Open Tabs; GLib/GIO-only
                     folder.c, fileops.c, gitmarks.c, icons.c
  search/            the Search panel, project-wide replace; searchmatch.c is its GLib-only part
  popups/            the picker, quick open, context menus
  dialogs/           the Settings, Keyboard Shortcuts and About windows
  project/           switching folders, the session, user files, build / run / tasks (run/), Python
                     and Rust toolchains, langenv.c (GLib-only)
  plugins/           plugins: manifests, palette commands and shortcuts, running them
  update/            the new-release check
  ipc/               requests from scripts and tools over $TCODE_SOCKET (socket.c)
styles/              the editor's colors: Dark Classic as a GtkSourceView style scheme
snippets/            the built-in snippets
plugins/             word-count, an example plugin (found when t-code runs from the source tree)
docs/                features.md, plugins.md (manifest and socket protocol), tasks.md, memory.md,
                     performance.md, packaging.md, screenshots
tests/               make test: editing, the language server client (with a fake server), the
                     debugger, search, the Files panel, Source Control, a whole window, plugins,
                     the update check, crash recovery and the GLib-only modules; make test-asan:
                     the ones without a display under ASan
tools/               measuring memory and CPU: the running process tree, a headless t-code, clangd
desktop/             app menu entry template and the app icon
packaging/           install.sh, uninstall.sh, build.sh, test.sh: Fedora, Arch, AUR, Debian, Void
                     Linux, Flatpak, Snap and AppImage packages, built into packaging/packages/
.clang-format        the code's style (2 spaces, 120 columns); clangd formats with it
LICENSE
Makefile             VERSION, make install; also writes build/compile_commands.json, so clangd
                     understands this code
```
