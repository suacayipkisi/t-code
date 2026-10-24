#!/usr/bin/env bash
# Builds the source-based AUR recipe in the Arch Linux container.
set -euo pipefail

pacman -Syu --noconfirm --needed base-devel gtk4 gtksourceview5 json-glib vte4 md4c clang git \
  ttf-cascadia-code hicolor-icon-theme
useradd -m builder
work=/home/builder/t-code-aur
install -d -o builder -g builder "$work"
cp /src/packaging/aur/PKGBUILD /src/packaging/aur/.SRCINFO "$work/"
chown -R builder:builder "$work"
su builder -c "cd '$work' && makepkg --printsrcinfo > .SRCINFO.generated &&
  diff -u .SRCINFO .SRCINFO.generated && rm .SRCINFO.generated"
if [[ -f "/out/t-code-$VERSION.tar.gz" ]]; then
  cp "/out/t-code-$VERSION.tar.gz" "$work/t-code-$VERSION.tar.gz"
  local_sum="$(sha256sum "$work/t-code-$VERSION.tar.gz" | cut -d' ' -f1)"
  sed -i "s/^sha256sums=.*/sha256sums=('$local_sum')/" "$work/PKGBUILD"
fi
su builder -c "cd '$work' && makepkg --noconfirm --cleanbuild"
cp "$work"/*.pkg.tar.* /out/
[[ -z "${HOST_OWNER:-}" ]] || chown -R "${HOST_OWNER}" /out
