#!/usr/bin/env bash
# Build the pinned release, or replace only its source with a working tree for CI.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source_dir=
out="$here/../packages/flatpak"
usage() {
  echo "Usage: bash packaging/flatpak/build.sh [--local-source DIR] [OUTPUT_DIR]" >&2
  exit 2
}
if [[ "${1:-}" == --local-source ]]; then
  (($# >= 2)) || usage
  source_dir="$(cd "$2" && pwd)"
  shift 2
fi
(($# <= 1)) || usage
if (($#)); then
  [[ "$1" != -* ]] || usage
  out=$1
fi
for command in flatpak flatpak-builder jq; do
  command -v "$command" >/dev/null || { echo "Missing build prerequisite: $command" >&2; exit 1; }
done
version="$(jq -er '."x-tcode-version"' "$here/io.github.suacayipkisi.tcode.json")"
arch="${FLATPAK_ARCH:-$(flatpak --default-arch)}"
case "$arch" in
  x86_64 | aarch64) ;;
  *) echo "Unsupported Flatpak architecture: $arch" >&2; exit 1 ;;
esac
if [[ -n "$source_dir" ]]; then
  read -r key operator source_version < "$source_dir/Makefile"
  [[ "$key" == VERSION && "$operator" == ':=' && "$source_version" == "$version" ]] || {
    echo "Local source Makefile must declare VERSION := $version" >&2
    exit 1
  }
fi
mkdir -p "$out"
out="$(cd "$out" && pwd)"
[[ "$out" != "$here" && "$out" != "$source_dir" ]] || {
  echo "The output directory must not be the package directory or local source root" >&2
  exit 1
}
skip_output=
if [[ -n "$source_dir" && "$out" == "$source_dir/"* ]]; then
  skip_output="${out#"$source_dir"/}"
fi
manifest_dir="$here"
source_path=
packaging_path="$here"
staging=
if [[ -n "$source_dir" ]]; then
  source_path="$source_dir"
  if [[ "$here" != "$source_dir" && "$here" != "$source_dir/"* ]]; then
    staging="$(mktemp -d)"
    mkdir -p "$staging/source" "$staging/packaging"
    tar_excludes=(--exclude=.git --exclude=build --exclude=.flatpak-builder --exclude=packaging/packages)
    if [[ -n "$skip_output" ]]; then
      tar_excludes+=(--exclude="$skip_output")
    fi
    tar -C "$source_dir" "${tar_excludes[@]}" -cf - . | tar -C "$staging/source" -xf -
    cp -a "$here/." "$staging/packaging/"
    manifest_dir="$staging"
    source_path="$staging/source"
    packaging_path="$staging/packaging"
  else
    manifest_dir="$source_dir"
  fi
fi
manifest_tmp="$(mktemp "$manifest_dir/.io.github.suacayipkisi.tcode.XXXXXX")"
manifest="$manifest_tmp.json"
mv "$manifest_tmp" "$manifest"
trap 'rm -f "$manifest"; [[ -z "$staging" ]] || rm -rf "$staging"' EXIT
skip_manifest=
if [[ -n "$source_dir" && "$manifest_dir" == "$source_dir" ]]; then
  skip_manifest="${manifest#"$source_dir"/}"
fi

# Keep every local manifest source within the manifest directory for --sandbox builds.
jq --arg packaging "$packaging_path" --arg source "$source_path" \
  --arg output "$skip_output" --arg manifest "$skip_manifest" '
  .modules |= map(
    if .name == "t-code" then
      .sources |= map(
        if .["x-tcode-source"] == true and $source != "" then
          {
            "type": "dir",
            "path": $source,
            "skip": ([".git", "build", ".flatpak-builder", "packaging/packages", $output, $manifest] |
                     map(select(length > 0)))
          }
        elif .type == "file" and has("path") then
          .path = ($packaging + "/" + .path)
        else . end
      )
    else . end
  )
' "$here/io.github.suacayipkisi.tcode.json" > "$manifest"

flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak-builder --user --install-deps-from=flathub --force-clean \
  --disable-rofiles-fuse --sandbox --arch="$arch" \
  --state-dir="$out/state" --repo="$out/repo" \
  "$out/build" "$manifest"
bundle="$out/t-code-$version-$arch.flatpak"
flatpak build-bundle --arch="$arch" \
  --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$out/repo" "$bundle" io.github.suacayipkisi.tcode stable
printf 'Bundle: %s\nRepository: %s\n' "$bundle" "$out/repo"
