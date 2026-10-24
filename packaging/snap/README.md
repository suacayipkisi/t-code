# Snap package

This directory packages t-code 0.3.1 as `t-code` for Linux amd64, with desktop/application ID `io.github.tcode`. It owns no shared packaging integration.

## Build

Use a Linux amd64 host with Snapcraft 9, working snapd, GNU tar, `sha256sum` and `unsquashfs` (`squashfs-tools`). The default build uses Snapcraft's managed LXD environment for Ubuntu 26.04 LTS (`core26`), not a Snapcraft OCI image. GitHub Actions instead uses an Ubuntu 26.04 runner with destructive mode, which matches `core26` and avoids relying on nested LXD networking. There is no dependency on a published `snapcraft-rocks:core26` image. Ordinary unprivileged Docker/Podman containers are not a supported build or install-test environment.

Example setup on a dedicated Ubuntu CI runner with passwordless sudo:

```sh
sudo apt-get update
sudo apt-get install -y squashfs-tools xvfb xauth dbus-x11
sudo snap install snapcraft --classic --channel=9.x/stable
sudo snap install lxd
sudo lxd init --auto
sudo usermod -aG lxd "$USER"
sg lxd -c 'bash packaging/snap/build.sh "$PWD" "$PWD/packaging/packages/snap"'
bash packaging/snap/test.sh "$PWD/packaging/packages/snap/t-code_0.3.1_amd64.snap"
```

Skip installation/init commands for already provisioned tools. Membership in `lxd` grants administrative access to the host; use an isolated CI runner. `--classic` above applies only to the **Snapcraft build tool**, never to t-code. Recent snapd with core26 support and access to the Snap Store/Ubuntu archives are required. `SNAPCRAFT_BUILD_MODE=destructive` is reserved for a disposable Ubuntu 26.04 host because Snapcraft may install build dependencies there.

The build interface is:

```sh
bash packaging/snap/build.sh /absolute/source/tree /absolute/output/directory
```

It accepts a checkout or an extracted source release, requires the manifest version to match `Makefile`, and copies source to a temporary workspace excluding existing build/package output. By default it uses LXD:

```sh
SNAPCRAFT_BUILD_INFO=1 snapcraft pack --use-lxd --output /absolute/output/directory/
```

For a disposable Ubuntu 26.04 host, set `SNAPCRAFT_BUILD_MODE=destructive`; the helper verifies the host version and runs Snapcraft with root privileges. This mode changes the host by installing build dependencies. The package workflow uses it on GitHub's `ubuntu-26.04` runner.

Output names are deterministic:

- `t-code_0.3.1_amd64.snap`
- `SHA256SUMS`
- `build-environment.txt`: Snapcraft and snapd versions, plus LXD version or host Ubuntu release
- `build-manifest.yaml`: Snapcraft's embedded build manifest, including resolved package/build-snap provenance

The recipe fixes the app version, base, architecture and GCC major, and fetches no floating application source. Ubuntu security updates and GNOME/base/content snap revisions still change over time. Keep the manifest and environment record with each artifact; this is not a claim of byte-for-byte reproducibility across archive or Store updates. For locked release rebuilds, retain the resolved apt packages and snap revisions in a controlled package/snap proxy or build environment.

The Snapcraft recipe adds Ubuntu Resolute's official `main` and `universe` archive as a package source and pins its Ubuntu Archive Automatic Signing Key fingerprint. APT verifies repository metadata and packages against that key; never omit the `key-id` to silence Snapcraft's validation.

The `gnome` extension supplies desktop integration, GTK4 and the desktop runtime. core24's GNOME 46 runtime cannot satisfy GtkSourceView 5.16's GTK >= 4.17 requirement without rebuilding GTK too; core26 avoids that additional stack. The recipe checks GtkSourceView >= 5.16 and VTE >= 0.78 during its build.

## Install and smoke test

```sh
bash packaging/snap/test.sh /absolute/path/to/t-code_0.3.1_amd64.snap
```

Run as a normal user with passwordless sudo on a disposable host. The script refuses to replace an existing t-code installation and refuses partial confinement. It installs the base/content providers, installs the local artifact with `snap install --dangerous` (unsigned package, still **strict**), and connects the declared home/network/content interfaces. It leaves installed packages in place for inspection.

Without a display it starts Xvfb; without a session bus it starts `dbus-run-session`. On a desktop it uses the existing session. To exercise X11 explicitly, set `GDK_BACKEND=x11`; to exercise Wayland, run from a real Wayland session with `GDK_BACKEND=wayland`. The default CI headless run does not prove Wayland behavior.

The installed smoke suite runs through `TCODE_SNAP_SMOKE_TEST=1 snap run t-code`, so the normal GNOME command chain, environment helper and **same app security profile** apply. It checks:

- Installed version, desktop `%F` file opening metadata, app ID, icon and editor style.
- Bundled tools on PATH, C23/C++/Clang compilation and execution, Git commit in a nonhidden host-home project, Make, CMake/Ninja and Meson builds, and GDB's embedded Python.
- Opening a project and C file in GTK, one clangd process, reading/editing/saving through the runtime socket, Markdown preview/IPC responsiveness, clean SIGTERM shutdown and socket removal.

It does not claim a successful ptrace/debug session or exercise every interactive control/plugin. Any subprocess/tool/build failure fails the smoke command. No build, test or formatter was run while authoring this package slice.

## Files, tools and confinement

- `home` grants access to user-owned projects below nonhidden host-home directories, for example `~/Projects/repo`. Nested `~/Projects/repo/.git` and `.t-code` are accessible. **Top-level** hidden paths such as `~/.git`, `~/.gitconfig`, `~/.ssh`, `~/.config` and `~/.projects` are not granted. Projects outside home, including arbitrary `/mnt`, `/media` and system trees, are not granted by this recipe. File picker portals do not provide general recursive project/toolchain access outside those permissions.
- Configuration and plugins live at `~/snap/t-code/common/.config/t-code/` and its `plugins/` subdirectory; Git's private global configuration is `~/snap/t-code/common/.config/git/config`. Data/cache also use `SNAP_USER_COMMON`; host `~/.gitconfig` and native `~/.config/t-code` configuration/plugins are not imported. Install plugins explicitly into the snap's directory. Scripts should use `/usr/bin/env python3` or the bundled shell, not a hard-coded host interpreter path. Plugins, terminals, build tools and built programs inherit the same confinement and can access only the snap's granted files/interfaces.
- `XDG_RUNTIME_DIR` remains snapd's `/run/user/<uid>/snap.t-code`. t-code creates `t-code-<pid>.sock` there and passes its full path as `TCODE_SOCKET` to its terminal/plugins. It is not relocated to the host's top-level runtime directory. Other snaps/native t-code instances do not share this runtime namespace; external clients must use the actual snap-specific path and remain subject to socket mediation.
- The GNOME extension provides X11, Wayland, desktop integration and OpenGL interfaces. Desktop files preserve `%F`, the new-window action, `StartupWMClass=io.github.tcode` and the shipped SVG icon. No unrestricted host filesystem interface, host command launcher, classic/devmode mode, `system-files`, `personal-files`, `process-control` or `system-trace` grant is requested.
- GCC 15, G++, Clang/clangd/clang-format/clang-tidy, Git/SSH/CA certificates/curl, ripgrep, Make/CMake/Meson/Ninja/pkgconf, Python and GDB are bundled. Their files are isolated under `toolchain/`, preserving a complete C/C++ sysroot from the GNOME extension's library cleanup. Compiler wrappers select that sysroot; Python, Git helpers and certificate paths point into the same tree. The terminal uses bundled Bash even if the user's host shell is unavailable. Host executables/libraries, SSH agents and credentials are not implicitly exposed.
- Standard C/C++ headers and libraries are bundled. Extra project development libraries, Go/Rust/Java/Node runtimes and their optional language servers are **not** bundled; installing those on the host does not make them available inside the snap. Projects requiring them need a separately packaged runtime/toolchain or a native package. Build outputs target the bundled Ubuntu 26.04 userspace, not an arbitrary host distribution ABI. Git identity and credentials must be configured inside the snap or at project scope.
- **F5 debugging is unavailable under the default strict policy.** GDB is bundled with Python/DAP support, but launching a debuggee requires ptrace that the default sandbox denies. A broad privileged tracing grant is not an acceptable default for this package, so the recipe does not request one or silently switch confinement modes. Standalone GDB metadata/Python commands still work; the smoke checks those, not debugging.
- Store publication requires ownership/registration of the `t-code` snap name, publisher credentials, and Store review. Local unsigned install does not establish publishability or automatic interface approval. The package does not bypass snapd's update management; the in-app native-package updater cannot modify this read-only snap.

## Shared integration hand-off

Call `build.sh <source-tree> <output-dir>` from a native Snapcraft/LXD job, or set `SNAPCRAFT_BUILD_MODE=destructive` on a disposable Ubuntu 26.04 runner. Collect the four output files, then call `test.sh <snap-file>` on a clean fully confined Linux runner. Capture its final `snap list`/`snap connections` output with CI logs for runtime revision provenance. Do not route this target through the existing ordinary OCI container helper. Release version checks should include `version` in `snapcraft.yaml`; the smoke compares the binary with snapd's `SNAP_VERSION` rather than duplicating the version.
