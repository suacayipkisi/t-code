#!/usr/bin/env bash
# Pins packaging/flatpak/io.github.tcode.json's t-code source (and the metainfo's screenshot) to the
# v<VERSION> tag's commit, with the archive's sha256. Run it after the tag is pushed, together with
# packaging/aur/update-checksum.sh: the release packages are built from the working tree
# (build.sh --local-source), so a stale pin only shows when the manifest is built on its own
# (Flathub), and then it builds the old release.
# Usage: packaging/flatpak/update-source.sh [version]   (default: the Makefile's VERSION)
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

version="${1:-$VERSION}"
manifest="$PACKAGING/flatpak/io.github.tcode.json"
metainfo="$PACKAGING/flatpak/io.github.tcode.metainfo.xml"

declared="$(sed -n 's/.*"x-tcode-version": "\([^"]*\)".*/\1/p' "$manifest")"
[[ "$declared" == "$version" ]] || die "The manifest says x-tcode-version $declared, not $version: bump it first"
commit="$(git -C "$ROOT" rev-parse --verify --quiet "v$version^{commit}")" ||
  die "No v$version tag here: pull the tags (git fetch --tags) or push it first"
url="https://codeload.github.com/suacayipkisi/t-code/tar.gz/$commit"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
info "Downloading $url"
curl -fsSL -o "$work/source.tar.gz" "$url" || die "No archive for $commit on GitHub: is the tag pushed?"
inside="$(tar -xzOf "$work/source.tar.gz" "t-code-$commit/Makefile" 2>/dev/null | sed -n 's/^VERSION *:= *//p')"
[[ "$inside" == "$version" ]] || die "The archive of $commit has VERSION ${inside:-(none)}, not $version"
sum="$(sha256sum "$work/source.tar.gz" | cut -d' ' -f1)"

# Only the source marked "x-tcode-source" changes; the rest of the file stays as written.
python3 -I - "$manifest" "$url" "$sum" <<'EOF'
import re, sys
path, url, digest = sys.argv[1:]
text = open(path).read()
pattern = re.compile(r'("url": ")[^"]*(",\s*"archive-type": "tar-gzip",\s*"sha256": ")[0-9a-f]*(",\s*"x-tcode-source": true)')
text, count = pattern.subn(lambda m: m.group(1) + url + m.group(2) + digest + m.group(3), text)
if count != 1:
    sys.exit(f"{path}: expected one x-tcode-source archive, found {count}")
open(path, "w").write(text)
EOF
sed -i "s|raw.githubusercontent.com/suacayipkisi/t-code/[0-9a-f]\{40\}/|raw.githubusercontent.com/suacayipkisi/t-code/$commit/|" "$metainfo"
grep -q "$sum" "$manifest" || die "Could not update $manifest"
info "Flatpak source pinned to v$version: $commit (sha256 $sum)"
