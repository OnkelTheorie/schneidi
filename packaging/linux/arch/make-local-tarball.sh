#!/bin/sh
# Packs the current git checkout (HEAD, committed files only) as the source tarball the PKGBUILD expects, so
# `makepkg --skipchecksums` builds the local state instead of downloading the release. Run from anywhere inside
# the repository; the tarball lands next to the PKGBUILD.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(git -C "$here" rev-parse --show-toplevel)
ver=$(sed -n 's/^pkgver=//p' "$here/PKGBUILD")
git -C "$top" archive --format=tar.gz --prefix="schneidi-$ver/" -o "$here/schneidi-$ver.tar.gz" HEAD
echo "$here/schneidi-$ver.tar.gz (HEAD $(git -C "$top" rev-parse --short HEAD))"
