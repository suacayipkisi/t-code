# Performance

How t-code stays light, and how to measure it. The [README](../README.md#performance) has the
numbers; [memory.md](memory.md) is the plan for a report of high memory use.

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
tools/measure.sh [-n 3] [tree]       # the table above: the same 25 files, each tab shown, several runs
tools/clangd-mem.py --churn 3        # clangd alone: resident memory vs what it says it uses
make test-asan                       # the tests without GTK under AddressSanitizer, failing on any leak
```

The tests also check memory: `tests/test_lsp.c` opens and closes a file 200 times against a fake
language server and fails if the heap grows by 8 KB (it grows by a few hundred bytes; a leak of 130
bytes a round fails it), and the package tests check that one clangd serves all C/C++ editors with
the arena limit.

