#!/usr/bin/env bash
# Builds the t-code AppImage into packaging/packages/appimage/, by hand only: releases no longer
# ship one (build.sh and test.sh leave it out). A single file without an install (t-code, GTK4,
# GtkSourceView, VTE and what they need are bundled with linuxdeploy's GTK plugin), built in a
# Debian container from make_tarball's tree by packaging/appimage/build-in-container.sh.
# Usage: packaging/appimage.sh   (podman or docker like packaging/build.sh)
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

IMAGE["appimage"]=docker.io/library/debian:trixie
out="$OUT/appimage"
check_versions
rm -rf "$out" && make_tarball "$out"
info "Building the AppImage in ${IMAGE[appimage]}"
run_in_container appimage "$out" bash /src/packaging/appimage/build-in-container.sh
rm -f "$out/t-code-$VERSION.tar.gz"
[[ -e "$out/t-code-$VERSION-x86_64.AppImage" ]] && (cd "$out" && sha256sum -- * > SHA256SUMS)
