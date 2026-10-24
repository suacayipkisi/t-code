# Memory: what t-code costs, and a 250–300 MB idle report (Milis Linux)

A report came in from Milis Linux: t-code idle at 250–300 MB. On Fedora 44, `tools/headless.sh`
with the same kind of session (25 files) puts the window plus its helpers at about a third of that
(clangd not counted). The difference is worth finding before writing more memory work. This is the
plan to find it.

## 1. Split the number first

`tools/proc-report.sh 10` reports the running process tree by program, in PSS (shared pages are
split between the processes, so the rows add up):

* **t-code's own window**: about 57–83 MB in Fedora's runs (70 MB for 0.3)
* **clangd**: usually the biggest when C / C++ files are open, 345–367 MB in Fedora's runs *with
  25 of this repository's files open*, and nothing at all when none is open (`lsp/client.c` stops
  it 30 seconds after its last file closes). Since 0.3.1, `pylsp`, `gopls` and `rust-analyzer` show
  up as their own rows the same way, and so do `typescript-language-server` (with the `tsserver`
  it runs under node), the JSON and YAML servers (node) and `marksman`. The JSON server is given no
  generated or big JSON file (a compile database, a lock file, over 512 KB) and downloads no schema.
* **the shells**: a few MB each
* the 0.1.1-era helpers (a micro process per tab, 50 helper processes) are gone since 0.2

Most "t-code uses 300 MB" reports turn out to be the language server's row. If the window is the
big one and the servers are at zero, the rest of this plan is about the window; if clangd is the
big one, it is about clangd on that system (step 3).

## 2. Cheap questions first (no code)

* `t-code --version`: which version the report was made with, to know whether 0.3's memory fixes
  are in (`malloc_trim` when a tab closes, clangd's `--pch-storage=disk`, its two malloc arenas)
* **PSS or RSS?** RSS counts shared pages in full in every process (GTK, fonts, libraries), so
  tools that show RSS count them two or three times. Compare only PSS numbers
  (`tools/proc-report.sh`).
* **Which files were open while idle?** A session that restores 25 tabs is not an empty start; the
  servers only know the tabs that were shown.
* **Fonts:** the editor's Cascadia Code and the Files panel's Nerd Font icons go through
  fontconfig's caches; a system with an unusual or very large font setup shows a bigger window.
  Compare once with a plain monospace font picked in Settings.

## 3. System specifics worth asking for

* **glibc or musl?** (`ldd "$(command -v t-code)"`). Two of t-code's memory measures only work
  on glibc: `GLIBC_TUNABLES=glibc.malloc.arena_max=2` for clangd and `malloc_trim(0)` after a tab
  closes. On musl they do nothing, and clangd built on musl also has no `--malloc-trim`; a musl
  system's numbers then show the allocator's behavior, not a t-code leak.
* **clangd's version and flags:** `ps -o args= -C clangd` should show `--background-index
  --pch-storage=disk -j=2 --limit-results=50`; an old clangd (a distribution that ships clangd 14,
  say) uses noticeably more memory for the same files.
* **The terminals:** compare the window's PSS with the shell panel shown (`Ctrl+J`) and hidden;
  each `VteTerminal` keeps its scrollback and its own glyph cache, and the difference is the
  terminals' share.

## 4. If the window itself is still big

* `make test-asan`: the tests that run without a display fail on any leak (LeakSanitizer). A
  leak in t-code's own heap is a bug, and these tests are where it shows first.
* `valgrind --tool=massif` (or `heaptrack`) over a minute of idle names the biggest allocations,
  which is the only step that points at the cause when the window is at issue.
* If nothing stands out, the memory is spread out (font caches, VTE, GTK's theme and icons):
  start from the biggest row of step 1 and look into that one.
* `tools/clangd-mem.py --churn 3` measures clangd alone: its resident memory against what it
  says it uses (`$/memoryUsage`), and what it gives back after files close.

## What a report should carry

From one run, `tools/proc-report.sh 10 > report.txt`, plus one line each for: `t-code --version`,
whether `ldd` shows musl, how many tabs were open and shown, the editor and Files panel fonts
from Settings, and `echo $GDK_BACKEND`. The rows show which process holds the memory; the fix
starts there.
