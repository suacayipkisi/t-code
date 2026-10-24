# Flatpak package

This directory is self-contained: copy its contents into a Flathub manifest
repository without the shared `packaging/` scripts. The application ID is
`io.github.tcode`; the packaged version is **0.3.1**.

The release source is upstream tag `v0.3`, pinned to commit
`f594a9f2746292adb3a59afc26b0fcd541f899c6` and archive SHA-256
`6a1945f37c246e2f9f3d416f6bebc5aa0e9d262461c51e82c983da98dd04a99a`.
There is no upstream `v0.3.0` tag. Every archive has a checksum and the
fast_float Git source has both a tag and commit.

## Runtime and tools

The runtime and build SDK are both **`org.gnome.Sdk//50`**, a supported GNOME
branch based on Freedesktop SDK 25.08. Using the SDK as the installed runtime
is intentional for this IDE, rather than using `org.gnome.Platform` and
silently losing the compiler, headers, Git, and debugger. Flathub's
[GNOME Builder manifest](https://github.com/flathub/org.gnome.Builder/blob/master/org.gnome.Builder.json)
uses the same SDK-runtime pattern. An SDK runtime does not grant access to
host files, processes, or D-Bus services.

The runtime supplies GTK4, GtkSourceView 5, JSON-GLib, GCC/G++, libc development
headers, Git, Make, CMake, Meson, Ninja, pkg-config, Python, and GDB. The manifest
builds VTE's GTK4 library and md4c, plus VTE's fast_float and simdutf build
dependencies. It copies clangd, the LLVM shared libraries, and Clang resource
headers from `org.freedesktop.Sdk.Extension.llvm20//25.08` into `/app`, so users
do not need to install that extension to use C/C++ language support. The
extension is a build dependency, not a host-tool bridge.

Builds and programs use this SDK's tools and libraries. Host compiler paths,
host development libraries, and host virtual environments in an existing
`compile_commands.json` may not exist inside the sandbox; regenerate the
compilation database with the included tools. Extra language servers and
language toolchains such as Go, Rust, and Node.js are not bundled. Plugins
that require an absent interpreter or tool cannot run until a compatible
tool is made available inside the sandbox.

The SDK runtime is larger than the Platform runtime. Runtime and SDK-extension
branches receive upstream security updates: source pins and artifact names are
deterministic, but this helper does not promise byte-identical rebuilds across
runtime updates. Move the runtime and LLVM extension together when upgrading
the GNOME/Freedesktop base, before the selected branch reaches end of support.

## Build

Run on Linux, as a normal user, with Flatpak, flatpak-builder, jq, Git, and
working user namespaces. Native x86_64 and aarch64 builds are supported.
`FLATPAK_ARCH` can select either architecture when the runner supports it.

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

# Build the pinned tagged release.
bash packaging/flatpak/build.sh packaging/packages/flatpak

# PR CI: build this checkout, including uncommitted source changes.
bash packaging/flatpak/build.sh --local-source "$PWD" packaging/packages/flatpak
```

`--local-source` also accepts an extracted release/CI source tree. Its Makefile
must still declare `VERSION := 0.3.1`. Only the application archive is replaced;
dependency pins, permissions, and package-local metadata remain unchanged.
Existing object files, Git state, and output directories are excluded. No
source files are edited, and the checkout may be mounted read-only.

The output directory defaults to `packaging/packages/flatpak`. It contains:

- `t-code-0.3.1-<architecture>.flatpak`: an installable, unsigned bundle;
- `repo/`: an unsigned OSTree repository containing
  `app/io.github.tcode/<architecture>/stable`;
- `io.github.tcode.json`: the resolved-input manifest used by the helper;
- `build/` and `state/`: build output and dependency/build caches.

The helper adds the user-level `flathub` remote idempotently, installs missing
SDK build dependencies from it, builds without network access in build
commands, and exports both artifacts. Reusing an output directory replaces its `build/`
tree and same-named bundle; reserve the directory for this helper. A published
repository should be signed by the release infrastructure.

For a direct Flathub-style build, the checked-in manifest is also usable as-is:

```sh
flatpak-builder --user --install-deps-from=flathub --disable-rofiles-fuse \
  --force-clean --repo=flatpak-repo flatpak-build packaging/flatpak/io.github.tcode.json
```

The Makefile installs the existing `io.github.tcode.svg` icon and style scheme.
The package-local desktop entry exports file arguments with `%F` and includes
the New Window action. Flatpak rewrites that entry to use `--file-forwarding`
and `@@` markers. The installed MetaInfo includes the desktop ID, license,
release, and a screenshot from the pinned upstream commit.

## Permissions and project access

There are **no static filesystem grants**, no host-spawn service permission,
and no unrestricted session or system D-Bus permission.

- `wayland`, `fallback-x11`, `ipc`, and `dri` provide the GTK window and rendering.
- `network` supports Git remotes, language tools, and programs run in the
  terminal. Programs and plugins inherit this network permission.
- `allow=devel` permits debugging child programs with GDB. Host processes remain
  outside the process namespace; kernel/container ptrace restrictions still apply.
- `SHELL=/bin/bash` selects a shell included in the SDK instead of inheriting an
  unavailable host login shell.

Open a **whole project folder** using Open Folder (`Ctrl+Shift+O`). The existing
`GtkFileDialog` folder chooser uses the desktop portal inside Flatpak; its
directory grant gives the file tree, Git, clangd, and builds access to that
folder's descendants. A single-file grant does not grant access to sibling
files, a parent `.git` directory, or headers elsewhere. Folder symlinks that
lead outside granted locations still require separate access.

For command-line file/folder opening, use Flatpak's file forwarding rather
than passing an inaccessible raw host path:

```sh
flatpak install --user ./packaging/packages/flatpak/t-code-0.3.1-x86_64.flatpak
flatpak run --file-forwarding io.github.tcode @@ "$HOME/projects/example" @@
flatpak run --file-forwarding io.github.tcode @@ "$HOME/projects/example/main.c" @@
```

This requires a functioning document portal and file-chooser backend on the
host. A deployment without them can grant one project directory explicitly
instead of granting the entire home or host filesystem:

```sh
flatpak run --filesystem="$HOME/projects/example" io.github.tcode "$HOME/projects/example"
```

For a persistent, user-chosen grant use
`flatpak override --user --filesystem="$HOME/projects/example" io.github.tcode`.
Git worktrees, submodules, and external include trees need their referenced
locations granted too. The package does not expose `~/.ssh`, Git configuration,
credential stores, SSH/GPG agents, or host-installed executables. Configure
credentials within the private app environment or opt into narrowly scoped
permissions yourself. Source-control authentication that depends on those
host resources is unavailable by default.

## Plugins and control socket

Flatpak redirects `XDG_CONFIG_HOME` to its private app configuration directory.
With default host paths, the current checkout discovers user plugins at:

```text
~/.var/app/io.github.tcode/config/t-code/plugins/<plugin-id>/
```

The native installation's `~/.config/t-code/plugins` and host system plugins
are not imported. Plugin processes inherit the same project grants, included
tools, and sandbox restrictions as the editor. No extra filesystem permission
is needed for private plugins.

The editor creates `t-code-<pid>.sock` in the sandbox's `XDG_RUNTIME_DIR`.
Terminals and plugin subprocesses share that namespace and receive the real
`TCODE_SOCKET` path. The host runtime directory is not exposed. Do not assume
that a host process or a separate `flatpak run` instance can reach the socket;
use the editor's terminal/plugins or enter its existing Flatpak instance.
Cross-instance window discovery based on runtime sockets is subject to the
same namespace restriction.

The pinned upstream `v0.3` release predates the checkout's plugin system and
new bidirectional socket commands. The tagged package cannot include
unreleased features. Use `--local-source` to exercise those changes in PR CI,
and advance the release source pin when upstream tags a release containing
them. The smoke test uses the `preview` command, which exists in both trees.

## Isolated, display-less smoke test

```sh
bash packaging/flatpak/smoke-test.sh \
  "$PWD/packaging/packages/flatpak/t-code-0.3.1-x86_64.flatpak"
```

Required host tools are Flatpak, jq, desktop-file-utils, dbus-run-session,
xvfb-run, and xauth. Run as an unprivileged user. The helper creates a temporary
HOME, XDG directories, Flatpak user installation, D-Bus session, and Xvfb display.
It installs the bundle and its runtime from Flathub, checks the exported desktop
entry and its forwarding markers, and feeds `smoke-in-sandbox.py` into Python
inside the app's **normal** sandbox. It does not add test filesystem grants,
use `flatpak --devel`, or bypass Flatpak's sandbox.

The in-sandbox checks cover installed version/data/icon/metadata, developer tool
availability, compiling and running a C program, GDB launching a child program,
opening five C files, starting clangd, the private control socket, a Markdown
preview request, SIGTERM shutdown, socket cleanup, and GTK critical/crash logs.
Any failure exits nonzero; app logs are printed on a launch failure.
The temporary installation and settings are removed on exit.

For a disposable test container on a Linux runner:

```sh
podman build -t t-code-flatpak-test \
  -f packaging/flatpak/Containerfile packaging/flatpak
podman run --rm --privileged \
  -v "$PWD/packaging/packages/flatpak:/out:ro,Z" \
  t-code-flatpak-test /out/t-code-0.3.1-x86_64.flatpak
```

The image supplies an unprivileged `tester` user. The outer container needs
nested namespace/mount and ptrace support for Flatpak/bubblewrap and GDB;
`--privileged` enables those on a dedicated CI runner. Only the bundle output
is mounted, read-only. The application still runs inside Flatpak with the
manifest's permissions. A rootless runner cannot override a host policy that
disables unprivileged user namespaces; use a runner that permits them, rather
than disabling the app sandbox. Downloading the runtime requires network access.

This headless test does not interact with a host file-chooser portal, test
credential providers, or exercise the plugin UI. Desktop portal folder grants
and file forwarding need a separate real-desktop acceptance check. Release CI
should call `build.sh --local-source <source-tree> <out>` and then
`smoke-test.sh <out>/t-code-0.3.1-<architecture>.flatpak`; no shared script is
sourced. Build, test, and lint commands were not run while adding this package.
