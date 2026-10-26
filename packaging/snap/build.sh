#!/usr/bin/env bash
# Usage: bash packaging/snap/build.sh [source-tree] [output-directory]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source="$(cd "${1:-$here/../..}" && pwd)"
output="${2:-$source/packaging/packages/snap}"
mkdir -p "$output"
output="$(cd "$output" && pwd)"
version="$(sed -n 's/^VERSION *:= *//p' "$source/Makefile")"
[[ -n "$version" ]] || { echo 'No Makefile version' >&2; exit 1; }
grep -qx "version: '$version'" "$source/packaging/snap/snapcraft.yaml" || {
  echo 'Snap version and Makefile disagree' >&2
  exit 1
}
[[ "$(uname -m)" == x86_64 && "$(uname -s)" == Linux ]] || {
  echo 'Build on Linux amd64 with Snapcraft 9' >&2
  exit 1
}

build_mode="${SNAPCRAFT_BUILD_MODE:-lxd}"
case "$build_mode" in
  lxd)
    build_args=(--use-lxd)
    ;;
  destructive)
    grep -qx 'ID=ubuntu' /etc/os-release && grep -qx 'VERSION_ID="26.04"' /etc/os-release || {
      echo 'Destructive mode requires an Ubuntu 26.04 host matching core26' >&2
      exit 1
    }
    build_args=(--destructive-mode)
    ;;
  *)
    echo "Unsupported SNAPCRAFT_BUILD_MODE: $build_mode (use lxd or destructive)" >&2
    exit 1
    ;;
esac

work="$(mktemp -d)"

cleanup() {
  if [[ "$build_mode" == destructive ]]; then
    sudo rm -rf -- "$work"
  else
    rm -rf -- "$work"
  fi
}
trap cleanup EXIT
# Use a clean copy; never let host objects, previous packages or Snapcraft state enter the build.
tar -C "$source" --exclude='./.git' --exclude='./build' --exclude='./parts' \
  --exclude='./stage' --exclude='./prime' --exclude='./.snapcraft' \
  --exclude='./packaging/packages' --exclude='*.snap' -cf - . |
  tar -C "$work" -xf -
mkdir -p "$work/snap"
cp "$work/packaging/snap/snapcraft.yaml" "$work/snap/snapcraft.yaml"

{
  snapcraft --version
  snap list snapcraft
  snap version
  apt-cache policy libvte-2.91-gtk4-0
  if [[ "$build_mode" == lxd ]]; then
    lxc version
  else
    grep -E '^(PRETTY_NAME|VERSION_ID)=' /etc/os-release
  fi
} | tee "$output/build-environment.txt"
build_artifact="$work/t-code-ide_${version}_amd64.snap"
(
  cd "$work"
  if [[ "$build_mode" == destructive ]]; then
    SNAPCRAFT_BUILD_INFO=1 sudo --preserve-env=SNAPCRAFT_BUILD_INFO \
      snapcraft pack "${build_args[@]}" --output "$work/"
  else
    SNAPCRAFT_BUILD_INFO=1 snapcraft pack "${build_args[@]}" --output "$work/"
  fi
)
[[ -f "$build_artifact" ]] || {
  echo "Snapcraft did not produce $build_artifact" >&2
  exit 1
}
artifact="$output/${build_artifact##*/}"
cp -- "$build_artifact" "$artifact"
unsquashfs -cat "$artifact" snap/manifest.yaml > "$output/build-manifest.yaml"
(
  cd "$output"
  sha256sum "${artifact##*/}" > SHA256SUMS
)
printf '%s\n' "$artifact"
