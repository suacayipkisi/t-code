# Building the packages

The distribution packages are built in containers using `podman` or `docker`. The AUR recipe
builds in an Arch container. Flatpak needs Linux, `flatpak`, `flatpak-builder`, `jq`, and access to
Flathub. Snap needs Linux amd64, Snapcraft 9, and initialized LXD; its fully confined smoke test
requires a native snapd/AppArmor host, not a container.

The shared scripts require Bash 4 or newer; macOS's bundled Bash 3.2 is too old. Flatpak and Snap
builds require Linux even when native distribution packages can use containers.

```sh
packaging/build.sh                 # all formats; requires the tools above
packaging/build.sh aur             # AUR source package
packaging/build.sh flatpak         # Flatpak bundle and repository
packaging/build.sh snap            # strict Snap package (Linux amd64 + LXD)
packaging/test.sh                  # all package smoke tests on a prepared Linux host
packaging/test.sh aur flatpak      # choose package tests
```

Artifacts land in `packaging/packages/<format>/`; the release directory contains installable RPM,
Arch, DEB, XBPS, Flatpak and Snap files, Snapcraft build/dependency provenance, and a
combined `SHA256SUMS`. The AppImage is not part of the release any more; `packaging/appimage.sh` builds
one by hand. The AUR build artifact is validated but is not attached as a release package;
publish the recipe separately to AUR after account/repository setup.
On Fedora, the RPM is built directly with `rpmbuild`; install its build dependencies once with
`sudo dnf install rpm-build dnf-plugins-core && sudo dnf builddep packaging/fedora/t-code-ide.spec`.

`packaging/test.sh` installs each distribution package into a fresh container, resolves its
dependencies, checks installed files and starts t-code under Xvfb with clangd, a project, preview,
and socket interactions. Flatpak smoke installs the bundle in a temporary user installation and
launches it in its real sandbox. Snap smoke requires a disposable Linux host with strict
confinement and passwordless sudo; it installs the unsigned package and leaves it installed for
inspection. The package build also runs tests that do not need a display.

