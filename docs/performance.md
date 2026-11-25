# Performance

How t-code stays light, and how to measure it. The [README](../README.md#performance) has the
numbers; [memory.md](memory.md) is the plan for a report of high memory use.

clangd's share moves a lot between runs (its background index, what it has freed back); the rest
is steady. With 25 tabs open and only one shown, both come to about 60 MB besides clangd: 0.1.1
started micro (about 20 MB) for each tab shown, an editor now costs its file and a few MB in the
window. While idle, t-code uses no CPU: nothing polls, editors redraw only when something changes.

What keeps it small:

- **clangd gets a file once its tab is shown**, and stops 30 seconds after the last C or C++ file
  closes. It keeps at most five files open: the least recently used hidden tab is closed on the
  server (its AST freed) and opened again when you go back to it. What a server frees stays
  resident (fragmented) until it exits, so one that grew 150 MB (or a quarter, if more) above where
  it settled is restarted once files were closed and it has been quiet for 30 seconds.
- **clangd's own flags**: precompiled headers on disk (`--pch-storage=disk`), kept in
  `~/.cache/t-code/preambles` rather than `/tmp` (RAM on many systems, and a killed clangd leaves
  them there), two indexing threads, at most 50 results, and `--malloc-trim` on glibc.
- **clangd runs on a tuned allocator**
  (`GLIBC_TUNABLES=glibc.malloc.arena_max=1:glibc.malloc.mmap_threshold=65536`). clangd's own data
  is a fraction of what it holds: glibc keeps freed memory in an arena per thread, and after a big
  block is freed it moves its mmap threshold up to 32 MB, so later big blocks come from the heap
  and stay taken. On a C++ project with Eigen and 25 files, clangd accounted for 314 MB and held
  1000-1190 MB; one arena and a fixed 64 KB threshold keep it at about 550 MB, at the same speed.
- **Every one of these is a setting** (Settings: C / C++, clangd), with a "Plain clangd" button for
  clangd and glibc as they come.
- **Only what changed goes to clangd** (incremental sync), batched while typing.
- **No browser engine**: the Markdown preview is a text view, not WebKit (which started a web and
  a network process, 150-200 MB, per preview).
- **Freed memory goes back**: t-code calls glibc's `malloc_trim(0)` when a tab closes, a search
  ends and the project changes, and five seconds after start-up or the last tab opened. glibc keeps
  what start-up (the renderer's set-up) and loading files freed: on the desktop (Vulkan) the
  window went from 76 to 39 MB without tabs, from 81-88 to 57-59 MB with 25.
- **The Files panel reads only open folders** and frees a folder's rows when it closes. It does
  not watch build output (`build/`, `target/`, `node_modules/`, `.git/`, `.cache/`), and a burst of
  changes reads a folder once (250 ms). Saving a file reads nothing: only files coming, going or
  being renamed do.

How to measure it yourself (see `tools/`):

```sh
tools/proc-report.sh 10              # the running t-code: processes, memory and CPU by program
tools/headless.sh src/*/*.c          # build/t-code run invisibly with these files, then the same report
tools/measure.sh [-n 3] [tree]       # the table above: the same 25 files, each tab shown, several runs
tools/clangd-mem.py --churn 3        # clangd alone: resident memory vs what it says it uses
make test-asan                       # the tests without GTK under AddressSanitizer, failing on any leak
```

The tests also check memory: `tests/test_lsp.c` opens and closes a file 200 times against a fake
language server and fails if the heap grows by 8 KB (it grows by a few hundred bytes; a leak of 130
bytes a round fails it), and the package tests check that one clangd serves all C/C++ editors with
the arena limit.

