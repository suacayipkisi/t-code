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
  follow) and the README's key table.

---

## 5. Packaging Guidelines

T-Code packages are distributed in multiple formats (Native packages for Fedora/Arch/Debian/Void, AppImage, Flatpak, Snap, AUR).
* **Subprocess Safety:** T-Code spawns developer tools (terminal shells, `clangd`, `gcc`, `make`, `gdb`, `git`). Packaging scripts must **never** export a blanket `LD_LIBRARY_PATH` that could poison or break system compilers in the embedded terminal. RPATH (`$ORIGIN`) isolation must be preserved.
* Test packaging changes using `packaging/build.sh <format>` and `packaging/test.sh <format>`.

---

Thank you for helping make T-Code better!
