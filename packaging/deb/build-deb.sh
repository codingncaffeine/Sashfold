#!/usr/bin/env bash
# Builds the Debian package from a staged Linux download folder — the one
# tools/ci stages and the release ships as sashfold-<version>-linux-x64 —
# with dpkg-deb, which Debian, Ubuntu and Arch (the dpkg package) all have.
#
#   bash packaging/deb/build-deb.sh <staged folder> [<out dir>]
#
# Layout: the binary with its themes, blocklists note and icon under
# /opt/sashfold, since the engine looks for those beside the executable it
# was started as; a wrapper on PATH at /usr/bin/sashfold; the desktop entry
# and the icon sizes under /usr/share; the license as the copyright file.
# The only library the binary imports beyond libc is libm, which is libc6's
# too, so the dependency is libc6 at the newest symbol version the binary
# names — read off the binary here rather than assumed.
set -euo pipefail
staged="${1:?usage: build-deb.sh <staged folder> [<out dir>]}"
out="${2:-.}"
[ -x "$staged/sashfold" ] || { echo "build-deb: no executable at $staged/sashfold" >&2; exit 1; }
version=$(sed -n 's/^project(sashfold LANGUAGES CXX VERSION \([0-9.]*\))/\1/p' "$(dirname "$0")/../../CMakeLists.txt")
[ -n "$version" ] || { echo "build-deb: no version in CMakeLists.txt" >&2; exit 1; }
# The glibc floor: the highest GLIBC_ symbol version the binary references.
glibc=$(objdump -T "$staged/sashfold" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n -k3,3n -u | tail -1)
[ -n "$glibc" ] || { echo "build-deb: could not read the glibc symbol versions" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
root="$work/sashfold_${version}_amd64"
mkdir -p "$root/DEBIAN" "$root/opt/sashfold" "$root/usr/bin" "$root/usr/share/applications" "$root/usr/share/doc/sashfold"
cp "$staged/sashfold" "$root/opt/sashfold/"
cp -r "$staged/themes" "$staged/blocklists" "$staged/assets" "$root/opt/sashfold/"
rm -rf "$root/opt/sashfold/assets/icons"
cp "$(dirname "$0")/../linux/sashfold" "$root/usr/bin/sashfold"
cp "$(dirname "$0")/../linux/sashfold.desktop" "$root/usr/share/applications/"
if [ -d "$staged/assets/icons/hicolor" ]; then
    mkdir -p "$root/usr/share/icons"
    cp -r "$staged/assets/icons/hicolor" "$root/usr/share/icons/"
else
    mkdir -p "$root/usr/share/icons/hicolor/512x512/apps"
    cp "$staged/assets/icon.png" "$root/usr/share/icons/hicolor/512x512/apps/sashfold.png"
fi
cp "$staged/LICENSE" "$root/usr/share/doc/sashfold/copyright"
cp "$staged/README.md" "$root/usr/share/doc/sashfold/"
chmod 755 "$root/opt/sashfold/sashfold" "$root/usr/bin/sashfold"
chmod 644 "$root/usr/share/applications/sashfold.desktop"
size=$(du -sk "$root" --exclude=DEBIAN | cut -f1)
cat > "$root/DEBIAN/control" <<EOF
Package: sashfold
Version: ${version}
Section: web
Priority: optional
Architecture: amd64
Depends: libc6 (>= ${glibc})
Installed-Size: ${size}
Maintainer: codingncaffeine <codingncaffeine@users.noreply.github.com>
Homepage: https://sashfold.com
Description: A web browser engine written from scratch, every byte
 Sashfold is an HTML, CSS and JavaScript engine and a browser built first
 for the readable web, with its own parsers, layout, text rasterizer, image
 decoders, network stack and TLS client, linking only the operating
 system's own libraries. The window is a Wayland client of its own.
EOF
mkdir -p "$out"
dpkg-deb --build --root-owner-group "$root" "$out/sashfold_${version}_amd64.deb" > /dev/null
echo "$out/sashfold_${version}_amd64.deb (glibc floor ${glibc})"
