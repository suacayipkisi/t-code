VERSION := 0.3.2
PKGS    := gtk4 gtksourceview-5 json-glib-1.0 vte-2.91-gtk4 gio-unix-2.0 md4c
# t-code: src/main.c and the feature folders (src/window, src/editor, ...).
APP_SRC := $(wildcard src/*.c src/*/*.c)
APP_HDR := $(wildcard src/*.h src/*/*.h)
TEST_SRC := $(wildcard tests/*.c)
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c23
# Where 'make install' puts things. t-code finds its data next to itself, so DATADIR must be
# $(PREFIX)/share.
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
APP_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null) -Isrc -Ibuild

all: build/t-code build/compile_commands.json

APP_OBJ := $(patsubst src/%.c,build/obj/%.o,$(APP_SRC))
APP_DEP := $(APP_OBJ:.o=.d)

build/obj/%.o: src/%.c build/license.h Makefile | build
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -MMD -MP -c -o $@ $<

build/t-code: $(APP_OBJ) Makefile | build
	$(CC) $(CFLAGS) -o $@ $(APP_OBJ) $(LDFLAGS) $(shell pkg-config --libs $(PKGS))

-include $(APP_DEP)
# LICENSE as a C string for the About window (src/dialogs/about.c).
build/license.h: LICENSE | build
	sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&\\n"/' $< > $@

# How each file is compiled, so clangd (in t-code or another editor) finds the GTK, VTE and md4c headers.
build/compile_commands.json: Makefile $(APP_SRC) $(TEST_SRC) | build
	@( printf '['; sep=; \
	   for f in $(APP_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -DTCODE_VERSION=\\\\\\"%s\\\\\\" -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$(VERSION)" "$$f"; sep=,; done; \
	   for f in $(TEST_SRC); do \
	     printf '%s\n  {"directory": "%s", "file": "%s", "command": "cc %s -DTCODE_VERSION=\\\\\\"%s\\\\\\" -c %s"}' \
	       "$$sep" "$(CURDIR)" "$$f" "$(CFLAGS) $(APP_CFLAGS)" "$(VERSION)" "$$f"; sep=,; done; \
	   printf '\n]\n' ) > $@

# Tests: searchmatch.c, editor/detect.c, git/linediff.c and files/{fileops,folder,gitmarks}.c alone (GLib); the Search
# panel, editing, Source Control, the Files panel and About in a window (they need a display for some or all of
# their tests).
build/test-searchmatch: tests/test_searchmatch.c src/search/searchmatch.c src/search/searchmatch.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags gio-2.0) -o $@ tests/test_searchmatch.c src/search/searchmatch.c \
	  $(shell pkg-config --libs gio-2.0)

build/test-search-panel: tests/test_search_panel.c src/search/search.c src/search/searchmatch.c src/window/palette.c \
                         src/popups/contextmenu.c $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_search_panel.c src/search/search.c src/search/searchmatch.c \
	  src/window/palette.c src/popups/contextmenu.c \
	  $(shell pkg-config --libs $(PKGS))

build/test-linediff: tests/test_linediff.c src/git/linediff.c src/git/linediff.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags glib-2.0) -o $@ tests/test_linediff.c src/git/linediff.c \
	  $(shell pkg-config --libs glib-2.0)

build/test-detect: tests/test_detect.c src/editor/detect.c src/editor/detect.h src/editor/indentconf.c \
                   src/editor/indentconf.h src/editor/blocks.c src/editor/blocks.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags glib-2.0) -o $@ tests/test_detect.c src/editor/detect.c \
	  src/editor/indentconf.c src/editor/blocks.c \
	  $(shell pkg-config --libs glib-2.0)

# The editor page's files (with the language server client) are included by the tests through
# tests/editor_page.h, to reach their static functions. test-lsp runs itself with --fake-server as
# the language server.
PAGE_SRC := $(filter-out src/editor/tabs.c src/editor/editor.c src/editor/preview.c,$(wildcard src/editor/*.c)) \
            $(wildcard src/lsp/*.c) src/git/linediff.c src/window/palette.c tests/editor_page.h
build/test-editing: tests/test_editing.c tests/broadway.h $(PAGE_SRC) $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_editing.c $(shell pkg-config --libs $(PKGS))

build/test-lsp: tests/test_lsp.c $(PAGE_SRC) $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_lsp.c $(shell pkg-config --libs $(PKGS))

# The debugger's protocol client against gdb (skipped without one).
DAP_SRC := src/debug/dap.c src/lsp/channel.c src/lsp/json.c
build/test-debug: tests/test_debug.c $(DAP_SRC) src/debug/dap.h src/lsp/channel.h src/lsp/json.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags json-glib-1.0 gio-2.0) -o $@ tests/test_debug.c $(DAP_SRC) \
	  $(shell pkg-config --libs json-glib-1.0 gio-2.0)

build/test-scm: tests/test_scm.c src/git/scm.c src/git/gitstatus.c src/git/patch.c src/popups/contextmenu.c src/window/palette.c $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_scm.c src/git/scm.c src/git/patch.c src/popups/contextmenu.c src/window/palette.c \
	  $(shell pkg-config --libs $(PKGS))

build/test-gitpatch: tests/test_gitpatch.c src/git/patch.c src/git/patch.h | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags glib-2.0) -o $@ tests/test_gitpatch.c src/git/patch.c \
	  $(shell pkg-config --libs glib-2.0)

FILES_GIO := src/files/fileops.c src/files/folder.c src/files/gitmarks.c src/files/icons.c
build/test-fileops: tests/test_fileops.c $(FILES_GIO) $(FILES_GIO:.c=.h) | build
	$(CC) $(CFLAGS) -Isrc $(shell pkg-config --cflags gio-2.0) -o $@ tests/test_fileops.c $(FILES_GIO) \
	  $(shell pkg-config --libs gio-2.0)

# The Files panel; tests/test_files.c includes src/files/files.c for its static functions.
build/test-files: tests/test_files.c $(wildcard src/files/*.c) src/window/palette.c $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_files.c $(FILES_GIO) src/window/palette.c \
	  $(shell pkg-config --libs $(PKGS))

# A whole window (every module but main.c): how tabs open and close.
build/test-window: tests/test_window.c tests/broadway.h $(APP_SRC) $(APP_HDR) build/license.h | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o $@ tests/test_window.c \
	  $(filter-out src/main.c,$(APP_SRC)) $(shell pkg-config --libs $(PKGS))

build/test-about: tests/test_about.c src/dialogs/about.c $(APP_HDR) build/license.h | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o $@ tests/test_about.c \
	  $(shell pkg-config --libs $(PKGS))
build/test-plugins: tests/test_plugins.c src/plugins/plugins.c src/plugins/plugins.h $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_plugins.c src/plugins/plugins.c \
	  $(shell pkg-config --libs $(PKGS))

build/test-update: tests/test_update.c src/update/update.c src/update/update.h $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o $@ tests/test_update.c src/update/update.c \
	  $(shell pkg-config --libs $(PKGS))

build/test-recovery: tests/test_recovery.c src/editor/recovery.c src/editor/recovery.h $(APP_HDR) | build
	$(CC) $(CFLAGS) $(APP_CFLAGS) -o $@ tests/test_recovery.c src/editor/recovery.c \
	  $(shell pkg-config --libs $(PKGS))


test: build/test-searchmatch build/test-detect build/test-linediff build/test-fileops build/test-lsp build/test-debug \
      build/test-search-panel build/test-editing build/test-scm build/test-gitpatch build/test-files build/test-window build/test-about \
      build/test-plugins build/test-update build/test-recovery
	build/test-searchmatch
	build/test-detect
	build/test-linediff
	build/test-fileops
	build/test-lsp
	build/test-debug
	build/test-search-panel
	build/test-editing
	build/test-scm
	build/test-gitpatch
	build/test-files
	build/test-window
	build/test-about
	build/test-plugins
	build/test-update
	build/test-recovery

# The tests that need no display again, under AddressSanitizer and UBSan: out-of-bounds access, use
# after free, undefined behavior, and (LeakSanitizer, at exit) every block that was not freed.
ASAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
              -Wall -Wextra -std=c23 -Isrc
ASAN_ENV   := G_DEBUG=gc-friendly ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
              LSAN_OPTIONS=suppressions=$(CURDIR)/tests/lsan.supp:print_suppressions=0

test-asan: | build
	mkdir -p build/asan
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags gio-2.0) -o build/asan/test-searchmatch \
	  tests/test_searchmatch.c src/search/searchmatch.c $(shell pkg-config --libs gio-2.0)
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags glib-2.0) -o build/asan/test-detect \
	  tests/test_detect.c src/editor/detect.c src/editor/indentconf.c src/editor/blocks.c \
	  $(shell pkg-config --libs glib-2.0)
	$(ASAN_ENV) build/asan/test-searchmatch
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags glib-2.0) -o build/asan/test-linediff \
	  tests/test_linediff.c src/git/linediff.c $(shell pkg-config --libs glib-2.0)
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags glib-2.0) -o build/asan/test-gitpatch \
	  tests/test_gitpatch.c src/git/patch.c $(shell pkg-config --libs glib-2.0)
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags gio-2.0) -o build/asan/test-fileops \
	  tests/test_fileops.c $(FILES_GIO) $(shell pkg-config --libs gio-2.0)
	$(ASAN_ENV) build/asan/test-detect
	$(ASAN_ENV) build/asan/test-linediff
	$(ASAN_ENV) build/asan/test-gitpatch
	$(ASAN_ENV) build/asan/test-fileops
	$(CC) $(ASAN_FLAGS) $(shell pkg-config --cflags json-glib-1.0 gio-2.0) -o build/asan/test-debug \
	  tests/test_debug.c $(DAP_SRC) $(shell pkg-config --libs json-glib-1.0 gio-2.0)
	$(ASAN_ENV) build/asan/test-debug
	$(CC) $(ASAN_FLAGS) $(APP_CFLAGS) -o build/asan/test-plugins \
	  tests/test_plugins.c src/plugins/plugins.c $(shell pkg-config --libs $(PKGS) 2>/dev/null)
	$(ASAN_ENV) build/asan/test-plugins
	$(CC) $(ASAN_FLAGS) $(APP_CFLAGS) -DTCODE_VERSION='"$(VERSION)"' -o build/asan/test-update \
	  tests/test_update.c src/update/update.c $(shell pkg-config --libs $(PKGS) 2>/dev/null)
	$(ASAN_ENV) build/asan/test-update
	$(CC) $(ASAN_FLAGS) $(APP_CFLAGS) -o build/asan/test-recovery \
	  tests/test_recovery.c src/editor/recovery.c $(shell pkg-config --libs $(PKGS) 2>/dev/null)
	$(ASAN_ENV) build/asan/test-recovery

# System-wide install, used by the packages in packaging/ (install.sh installs into ~/.local).
install: build/t-code
	install -Dm755 build/t-code $(DESTDIR)$(BINDIR)/t-code
	install -Dm644 styles/dark-classic.xml $(DESTDIR)$(DATADIR)/t-code/styles/dark-classic.xml
	install -Dm644 desktop/io.github.tcode.svg \
	  $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/io.github.tcode.svg
	install -d $(DESTDIR)$(DATADIR)/applications
	sed 's|@BIN@|$(BINDIR)/t-code|' desktop/io.github.tcode.desktop.in \
	  > $(DESTDIR)$(DATADIR)/applications/io.github.tcode.desktop
	chmod 644 $(DESTDIR)$(DATADIR)/applications/io.github.tcode.desktop

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean test test-asan install
