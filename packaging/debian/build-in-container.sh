# Runs inside the Debian container (packaging/build.sh debian): builds the .deb from
# /out/t-code-$VERSION.tar.gz into /out, with packaging/debian/debian as the debian/ folder.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends build-essential devscripts equivs lintian
work=/tmp/build
mkdir -p "$work"
tar -C "$work" -xzf "/out/t-code-$VERSION.tar.gz"
cp "/out/t-code-$VERSION.tar.gz" "$work/t-code-ide_$VERSION.orig.tar.gz"
src="$work/t-code-$VERSION"
cp -r /src/packaging/debian/debian "$src/debian"
(cd "$src" && mk-build-deps --install --remove \
  --tool 'apt-get -y --no-install-recommends' debian/control)
(cd "$src" && dpkg-buildpackage -us -uc -b)
# What a Debian sponsor checks first: errors fail the build, warnings are only shown.
lintian --fail-on error "$work"/*.changes
cp "$work"/*.deb /out/
[[ -z "${HOST_OWNER:-}" ]] || chown -R "$HOST_OWNER" /out
