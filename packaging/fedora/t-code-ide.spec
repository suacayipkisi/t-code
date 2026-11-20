Name:           t-code-ide
Version:        0.5.1
Release:        1%{?dist}
Summary:        Lightweight C/C++ IDE: file tree, editor tabs with clangd and a shell
License:        MIT
URL:            https://github.com/suacayipkisi/t-code
# The upstream project and its archive are named t-code; the package is t-code-ide.
Source0:        %{url}/archive/v%{version}/t-code-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(gtksourceview-5)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(vte-2.91-gtk4)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(md4c)
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

Requires:       clang-tools-extra
Requires:       git-core
Requires:       cascadia-code-fonts
Requires:       hicolor-icon-theme
Requires:       librsvg2
Recommends:     ripgrep
Recommends:     gdb
Suggests:       mercurial
Suggests:       subversion
# The package was called t-code before 0.4.1.
Obsoletes:      t-code < 0.4.1-1
Provides:       t-code = %{version}-%{release}

%description
t-code is a lightweight C/C++ IDE in a GTK4 window: a project file tree,
GtkSourceView editor tabs sharing one clangd (diagnostics, completion,
parameter hints, go to definition, quick fixes), a terminal, a Search panel,
Source Control, git comparisons and Markdown previews. Projects are built,
run and debugged (with gdb) from the editor.

%package -n t-code-matlab
Summary:        MATLAB plugin for t-code: run .m files in a MATLAB session
BuildArch:      noarch
Requires:       t-code-ide = %{version}-%{release}
Requires:       python3

%description -n t-code-matlab
Runs .m files in a MATLAB session (matlab -nodesktop) in a t-code terminal
tab, one-off in batch, or a selection at a time; run errors show as
diagnostics in the editor. MATLAB itself is not included: it needs the
user's own licensed installation.

%package -n t-code-octave
Summary:        GNU Octave plugin for t-code: run .m files in an Octave session
BuildArch:      noarch
Requires:       t-code-ide = %{version}-%{release}
Requires:       python3
Recommends:     octave

%description -n t-code-octave
Runs .m files in a GNU Octave session (octave --no-gui) in a t-code terminal
tab, one-off in batch, or a selection at a time; run errors show as
diagnostics in the editor.

%prep
%autosetup -n t-code-%{version}

%build
%make_build build/t-code

%install
%make_install PREFIX=%{_prefix} BINDIR=%{_bindir} DATADIR=%{_datadir}
%{__make} install-plugin DESTDIR=%{buildroot} DATADIR=%{_datadir} PLUGIN=matlab
%{__make} install-plugin DESTDIR=%{buildroot} DATADIR=%{_datadir} PLUGIN=octave

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.suacayipkisi.tcode.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.suacayipkisi.tcode.metainfo.xml
%{buildroot}%{_bindir}/t-code --version | grep -qx 't-code %{version}'
# The other tests open windows and need a display.
%make_build build/test-searchmatch build/test-detect build/test-linediff
build/test-searchmatch
build/test-detect
build/test-linediff

%files
%license LICENSE THIRD-PARTY.md
%doc README.md
%{_bindir}/t-code
%{_mandir}/man1/t-code.1*
%dir %{_datadir}/t-code
%{_datadir}/t-code/styles/
%{_datadir}/t-code/snippets/
%dir %{_datadir}/t-code/plugins
%{_datadir}/applications/io.github.suacayipkisi.tcode.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg
%{_metainfodir}/io.github.suacayipkisi.tcode.metainfo.xml

%files -n t-code-matlab
%license LICENSE
%{_datadir}/t-code/plugins/matlab/

%files -n t-code-octave
%license LICENSE
%{_datadir}/t-code/plugins/octave/

%changelog
* Sat Nov 21 2026 suacayipkisi <konuki8523@gmail.com> - 0.5.1-1
- Fix: no crash when a plugin command ends at once, which a busy machine made likely

* Sat Nov 21 2026 suacayipkisi <konuki8523@gmail.com> - 0.5.0-1
- Testing panel: CTest, pytest, go test, cargo test and JUnit (GoogleTest cases too); run or debug a test from the panel or the editor gutter, go to its source
- More debugging: Python (debugpy), Go (Delve), Rust, lldb-dap as another C / C++ debugger; logpoints, a Memory View, attaching to a running process, opening a core dump
- Several shells as tabs in the Terminal panel (Ctrl+Shift+T); Build and Run use the first
- Source Control for Mercurial and Subversion next to git: conflicts, branches, hg shelve as stash, svn changelists; Initialize Repository offers all three; repositories nested in a project show too
- Plugin API tapi_1: socket commands for editing, asking, diagnostics and terminal tabs; plugins can bring language servers, Run, debuggers, color themes, language specs and snippets
- A Plugins window, panel and menu: switch plugins off, uninstall them, install from the registry; the registry index is signed, unreviewed plugins warn first
- Java plugin: jdtls, Run with errors and exceptions as diagnostics, JUnit tests (Maven, Gradle), debugging through java-debug, library sources
- Plugins for MATLAB and GNU Octave, also as the packages t-code-matlab and t-code-octave
- More plugins, written in C, C++, Go, Rust, JavaScript and Perl: CMake, ShellCheck, Format on Save, LaTeX and Typst, Spell Check, Make / just / Task / npm targets, a commit message helper, GitHub / GitLab permalinks, TODO Finder, Word Count, Pomodoro and text tools
- Process Monitor: t-code's processes with their CPU and memory, by owner
- A welcome window when there is no folder to open
- Save a file as a template; templates are offered again for new files
- Closing t-code ends the programs plugin terminal tabs started, and only those (MATLAB gets exit first)
- Objective-C (.m, .mm) goes to clangd; a .m file is MATLAB unless Objective-C sits next to it
- t-code's own SVG icons for files and the interface: no Nerd Font needed
- Panels give way when their side narrows; drag an edge to open a side
- Completion: Tab takes the proposal, Enter starts a new line
- Big repositories: quicker Outline, Source Control lists, conflict and semantic token scans
- Fix: big Markdown files no longer freeze the preview
- Fix: the Terminal and right panels keep their size in a small window (they could start hidden)
- Fix: Ctrl+J no longer brings back the side Ctrl+B hid
- Fix: CMakeLists.txt and Makefile are colored again
- Fix: the Outline shows the new tab's symbols after a tab switch
- Fix: search results stay sorted by path when grep does the search

* Sat Oct 31 2026 suacayipkisi <konuki8523@gmail.com> - 0.4.2-1
- Markdown preview: TeX math ($…$, $$…$$ and math blocks), GitHub alerts, :emoji:, footnotes, raw HTML as GitHub shows it, web and SVG images, mermaid diagrams when mmdc is installed
- Images open in a tab that shows them; PDFs, office documents, archives, audio, video and fonts open in the desktop's default app
- Hierarchy panel: with Follow on, a click on a function shows its callers or callees by itself
- While Ctrl is held, the name Ctrl+click goes to is underlined
- Autosave delay in the Editor settings (milliseconds, 1 second by default)
- A status bar button shows the text scale when it is not 100%%; a click resets it
- Fix: start-up no longer hangs on a log file of hundreds of MB
- Fix: lists with an open right-click menu (Source Control, Search, Run and Debug) no longer freeze the window
- Fix: no crash on an image or link with an empty target in the preview
- Fix: no VTE or GtkSourceView warnings from the terminal's folder and from completions

* Mon Oct 26 2026 suacayipkisi <konuki8523@gmail.com> - 0.4.1-1
- Renamed to t-code-ide; new application ID io.github.suacayipkisi.tcode
- Ships the AppStream metainfo and a man page
- A log file for warnings, opened from the Help menu
- Fix: undo a word typed at several cursors in one step
- Fix: a long breadcrumb no longer widens the editor
- Fix: the Outline panel forgets its widgets once they are gone
- Fix: socket requests are dropped while the window closes

* Mon Oct 26 2026 suacayipkisi <konuki8523@gmail.com> - 0.4.0-1
- Multiple cursors (add next occurrence, column selection, Alt+Click)
- Replace across the project, with a preview
- Snippets, built in and the user's own
- Inlay hints (hold Ctrl+Alt to turn them the other way)
- Outline panel, breadcrumbs, call and type hierarchy
- Git: switch and create branches, pull and fetch, stash
- Git: line blame, merge conflict accept links, hunk staging in the picker
- Git: comparisons show only the changes; commit message kept and recalled
- Language servers: JavaScript / TypeScript, JSON, YAML, Markdown, CMake
- Language servers: quick fixes in the hover, progress in the status bar, stop when idle
- Menu bar in the title bar, ruler column, Markdown preview scale
- Python interpreter and Cargo features / target picking
- Fixes: socket clients, recovery journal, Problems panel, AppImage libraries

* Sat Oct 24 2026 suacayipkisi <konuki8523@gmail.com> - 0.3.2-1
- Fix release URL launcher from update dialog
- Fix compile_commands.json string quote escaping for clangd
- AUR recipe source resolution fix

* Sat Oct 24 2026 suacayipkisi <konuki8523@gmail.com> - 0.3.1-1
- Plugins system (C shared libraries and UI contributions)
- Problems panel (workspace diagnostics, filtering, jump to source)
- Configurable tasks runner (.tcode/tasks.json, background jobs, terminal binding)
- Crash recovery (dirty buffer autosave and restore)
- Packaging: Flatpak, Snap, AUR support, reproducible AppImage builds
- Test suite enhancements and headless CI stability
- Contributor credits to dontworryplz

* Wed Oct 21 2026 suacayipkisi <konuki8523@gmail.com> - 0.3.0-1
- A debugger: gdb over DAP, breakpoints (with conditions), F5 / stepping, the
  Run and Debug panel (variables, watch, call stack, breakpoints), values
  under the pointer, a Disassembly View
- Quick fixes (Ctrl+.), Go to Symbol in File, Switch Source / Header, uses of
  the symbol at the cursor highlighted, #if branches left out dimmed
- Code folding and sticky scroll
- Color themes with previews, a square look, settings.ini and hand-editable
  theme files
- Panels that move between docks, a title bar button per panel, Open in New
  Window
- Right-click menus everywhere; Source Control commit details; comparisons
  open at the first change
- A save conflict bar; Run for Python, JavaScript, Rust, Go, Java and other
  projects

* Tue Oct 20 2026 suacayipkisi <konuki8523@gmail.com> - 0.2.1-1
- The Files panel is a native folder tree (GtkListView) instead of a terminal
  file manager: lazy folders, git colors, drag and drop, create / rename /
  delete / copy / move
- Temporary tabs (a click in the file tree previews a file), and a comparison is one tab
- Indentation from .editorconfig, .clang-format or the file itself, shown and
  changeable per project in the status bar
- clangd can be restarted from the Command Palette; a status bar hint when no
  compile flags are found
- Smarter Enter after an opening bracket, double click selects a name, other
  occurrences of a selected name are highlighted and marked beside the minimap
- Tab moves between a completed function's parameters

* Mon Oct 19 2026 suacayipkisi <konuki8523@gmail.com> - 0.2-1
- Editor tabs are GtkSourceView 5 views instead of micro terminals, with an
  in-process clangd client: problems underlined, completion, parameter hints,
  hover, go to definition, references, rename, formatting, semantic colors
- Find and replace bar, closing brackets and quotes, line commands, indent
  guides, a minimap marking problems, git's changes in the gutter
- Side-by-side comparisons stay aligned and scroll together
- Files, Source Control, Search and the shell can be moved between 12 docks
- Native Markdown preview (md4c in a text view): no WebKit
- Less memory: one clangd with at most five open documents, freed heap
  returned to the system, no process per editor
* Sat Oct 17 2026 suacayipkisi <konuki8523@gmail.com> - 0.1.1-1
- Much less memory and CPU: editors start when their tab is first shown, hidden
  editors stay idle, clangd runs with two malloc arenas, the Markdown preview's
  network process exits with the last preview
- Opening t-code no longer flashes clipboard windows or clears the clipboard
- Tests for the shared language servers, including a heap growth check

* Sat Oct 17 2026 suacayipkisi <konuki8523@gmail.com> - 0.1-1
- First release
