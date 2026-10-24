#!/usr/bin/env bash
# Puts the released archive's sha256 into packaging/aur/PKGBUILD and .SRCINFO.
# Run it after the v<VERSION> tag is pushed: the checksum is of GitHub's archive of that tag, which
# does not exist before. packaging/build.sh cannot catch a stale checksum (its AUR build uses the
# working tree's tarball and that one's checksum).
# Usage: packaging/aur/update-checksum.sh [version]   (default: the Makefile's VERSION)
source "$(dirname "${BASH_SOURCE[0]}")/../lib.sh"

version="${1:-$VERSION}"
pkgbuild="$PACKAGING/aur/PKGBUILD"
srcinfo="$PACKAGING/aur/.SRCINFO"
url="https://github.com/suacayipkisi/t-code/archive/refs/tags/v$version.tar.gz"

pkgver="$(sed -n 's/^pkgver=//p' "$pkgbuild")"
[[ "$pkgver" == "$version" ]] || die "PKGBUILD has pkgver=$pkgver, not $version: bump it first"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
info "Downloading $url"
curl -fsSL -o "$work/source.tar.gz" "$url" || die "No archive for v$version: is the tag pushed?"

# The archive must be the release it claims to be (the Makefile's VERSION inside it).
inside="$(tar -xzOf "$work/source.tar.gz" "t-code-$version/Makefile" 2>/dev/null | sed -n 's/^VERSION *:= *//p')"
[[ "$inside" == "$version" ]] || die "The v$version archive's Makefile says VERSION ${inside:-(none)}"

sum="$(sha256sum "$work/source.tar.gz" | cut -d' ' -f1)"
old="$(sed -n "s/^sha256sums=('\(.*\)')$/\1/p" "$pkgbuild")"
if [[ "$old" == "$sum" ]]; then
  info "Already up to date: $sum"
  exit 0
fi
sed -i "s/^sha256sums=('.*')$/sha256sums=('$sum')/" "$pkgbuild"
sed -i "s/^\(\s*sha256sums = \).*/\1$sum/" "$srcinfo"
grep -q "sha256sums=('$sum')" "$pkgbuild" || die "Could not update $pkgbuild"
grep -q "sha256sums = $sum" "$srcinfo" || die "Could not update $srcinfo"
info "sha256 ${old:-(none)} -> $sum in PKGBUILD and .SRCINFO"
