# Shared by build.sh and test.sh (sourced, not run).
set -euo pipefail

PACKAGING="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$PACKAGING")"
OUT="$PACKAGING/packages"
VERSION="$(sed -n 's/^VERSION *:= *//p' "$ROOT/Makefile")"
DISTROS=(fedora arch aur debian void void-musl flatpak snap)

# The images the packages are built and tested in.
declare -A IMAGE=(
  ["fedora"]=registry.fedoraproject.org/fedora:44
  ["arch"]=docker.io/library/archlinux:latest
  ["debian"]=docker.io/library/debian:trixie
  ["void"]=ghcr.io/void-linux/void-glibc-full:latest
  ["void-musl"]=ghcr.io/void-linux/void-musl-full:latest
  ["aur"]=docker.io/library/archlinux:latest
)

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*" >&2; }
die() { printf '\033[1;31mxx\033[0m  %s\n' "$*" >&2; exit 1; }

container_runtime() {
  if command -v podman >/dev/null; then echo podman
  elif command -v docker >/dev/null; then echo docker
  else die "Neither podman nor docker is installed"
  fi
}

# Runs a command inside a fresh container: the repo is mounted read-only at /src, $2 read-write at
# /out. Rootless podman's root is the calling user already; with docker, the build scripts give
# the files in /out back to the caller (HOST_OWNER).
run_in_container() { # <distro> <out dir> <command...>
  local distro=$1 out=$2 runtime owner=()
  shift 2
  runtime="$(container_runtime)"
  [[ "$runtime" == docker ]] && owner=(-e HOST_OWNER="$(id -u):$(id -g)")
  mkdir -p "$out"
  # Void's images have no bash, which the scripts here are written for.
  [[ "$distro" == void* ]] &&
    set -- sh -c 'xbps-install -Syu xbps >/dev/null && xbps-install -Sy bash >/dev/null && exec "$@"' sh "$@"
  "$runtime" run --rm \
    -v "$ROOT:/src:ro,Z" -v "$out:/out:Z" \
    -e VERSION="$VERSION" "${owner[@]}" \
    "${IMAGE[$distro]}" "$@"
}

# Every version string has to agree with the Makefile's.
check_versions() {
  local spec pkgbuild aur_pkgbuild aur_srcinfo changelog flatpak_version snap_version
  spec="$(sed -n 's/^Version: *//p' "$PACKAGING/fedora/t-code.spec")"
  pkgbuild="$(sed -n 's/^pkgver=//p' "$PACKAGING/arch/PKGBUILD")"
  aur_pkgbuild="$(sed -n 's/^pkgver=//p' "$PACKAGING/aur/PKGBUILD")"
  aur_srcinfo="$(sed -n 's/^[[:space:]]*pkgver = //p' "$PACKAGING/aur/.SRCINFO")"
  changelog="$(sed -n '1s/^t-code (\([^-]*\)-.*/\1/p' "$PACKAGING/debian/debian/changelog")"
  flatpak_version="$(sed -n 's/.*"x-tcode-version": "\([^"]*\)".*/\1/p' "$PACKAGING/flatpak/io.github.tcode.json")"
  snap_version="$(sed -n "s/^version: '\\([^']*\\)'/\\1/p" "$PACKAGING/snap/snapcraft.yaml")"
  [[ -n "$VERSION" ]] || die "No VERSION in Makefile"
  for v in "$spec" "$pkgbuild" "$aur_pkgbuild" "$aur_srcinfo" "$changelog" "$flatpak_version" "$snap_version"; do
    [[ "$v" == "$VERSION" ]] ||
      die "Version mismatch: Makefile $VERSION, Fedora $spec, Arch $pkgbuild, AUR PKGBUILD $aur_pkgbuild/.SRCINFO $aur_srcinfo, Debian $changelog, Flatpak $flatpak_version, Snap $snap_version"
  done
}

# t-code-<version>.tar.gz from the working tree: tracked files plus new ones git does not ignore,
# so uncommitted changes are packaged too, but never build/ or packages/.
make_tarball() { # <dir>
  local name="t-code-$VERSION" list
  list="$(mktemp)"
  (cd "$ROOT" && git ls-files -z -co --exclude-standard) |
    while IFS= read -r -d '' f; do [[ -e "$ROOT/$f" ]] && printf '%s\0' "$f"; done > "$list"
  mkdir -p "$1"
  tar -C "$ROOT" --null -T "$list" --sort=name --owner=0 --group=0 --numeric-owner \
    --transform "s,^,$name/," -czf "$1/$name.tar.gz"
  rm -f "$list"
}
