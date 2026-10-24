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
* **One module = one `.c` with a `.h` next to it** in its feature's folder (`src/editor/`,
  `src/lsp/`, ...). The `.h` documents what other modules may call; everything else is `static`.
  The `.c` includes its own header first.
* **GLib-only modules stay GLib-only** (`editor/detect.c`, `editor/indentconf.c`,
  `git/linediff.c`, `git/patch.c`, `search/searchmatch.c`, `lsp/compiledb.c`, `lsp/json.c`,
  `debug/dap.c`, `files/{fileops,folder,gitmarks,icons}.c`): they are tested without a display
  and under ASan.
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
