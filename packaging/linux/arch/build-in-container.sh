#!/bin/bash
# Builds the Arch package of a git ref (default HEAD, e.g. REF=v0.23.0 for a release) in a clean archlinux container (podman or docker), then installs it
# in a second fresh container and runs a smoke test. Works on any Linux host, no Arch needed.
#
#   packaging/linux/arch/build-in-container.sh [OUTDIR] [-- extra makepkg args, e.g. --nocheck]
#
# Env: REF (git ref to build, default HEAD), JOBS (default: nproc), CPUS (taskset list, e.g. 1-5,7-11; default: all), ENGINE (podman|docker).
# Result: OUTDIR/schneidi-<ver>-1-x86_64.pkg.tar.zst plus build.log and smoke.log.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(git -C "$here" rev-parse --show-toplevel)
ref=${REF:-HEAD}
ver=$(git -C "$repo" show "$ref:CMakeLists.txt" | sed -n 's/^project(schneidi VERSION \([0-9.]*\).*/\1/p')
out=${1:-$PWD/arch-$ver}
[[ $# -gt 0 ]] && shift
[[ ${1:-} == -- ]] && shift
engine=${ENGINE:-$(command -v podman >/dev/null && echo podman || echo docker)}
jobs=${JOBS:-$(nproc)}
image=schneidi-arch-build

mkdir -p "$out/work"
out=$(cd "$out" && pwd)
work=$out/work

# Build image: clean Arch with base-devel and namcap, unprivileged user "builder" (makepkg refuses root).
"$engine" build -q -t "$image" -f - "$here" <<'EOF'
FROM docker.io/library/archlinux:latest
RUN pacman -Syu --noconfirm --needed base-devel namcap git sudo && pacman -Scc --noconfirm
RUN useradd -m -u 1000 builder && echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder
EOF

# Source: the ref's committed files as the release tarball, the current PKGBUILD on the ref's version.
cp "$here/PKGBUILD" "$work/"
sed -i "s/^pkgver=.*/pkgver=$ver/; s/^pkgrel=.*/pkgrel=1/" "$work/PKGBUILD"
git -C "$repo" archive --format=tar.gz --prefix="schneidi-$ver/" -o "$work/schneidi-$ver.tar.gz" "$ref"
chmod -R a+rwX "$work"

# Rootless podman maps container uid 1000 to a sub-uid; keep-id makes it the caller so the files stay ours.
userns=()
[[ $engine == podman ]] && userns=(--userns=keep-id:uid=1000,gid=1000)
pin=()
[[ -n ${CPUS:-} ]] && pin=(taskset -c "$CPUS")

echo "Building schneidi $ver for Arch with $engine, -j$jobs -> $out"
start=$(date +%s)
"${pin[@]}" "$engine" run --rm "${userns[@]}" -u 1000 -v "$work:/work" -w /work \
    -e CMAKE_BUILD_PARALLEL_LEVEL="$jobs" -e MAKEFLAGS="-j$jobs" "$image" \
    bash -c 'namcap PKGBUILD; nice -n 19 makepkg -s --noconfirm --skipchecksums "$@" && namcap *.pkg.tar.zst' \
    makepkg "$@" 2>&1 | tee "$out/build.log"
echo "build: $(( $(date +%s) - start )) s" | tee -a "$out/build.log"

pkg=$(ls "$work"/schneidi-"$ver"-*.pkg.tar.zst | grep -v -- -debug | head -1)
mv "$pkg" "$out/"
pkg=$out/$(basename "$pkg")

# Smoke test in a fresh container: install with pacman (pulls the runtime dependencies), start both programs.
"$engine" run --rm -v "$pkg:/pkg/$(basename "$pkg"):ro" docker.io/library/archlinux:latest bash -c '
    set -e
    pacman -Syu --noconfirm >/dev/null
    pacman -U --noconfirm /pkg/*.pkg.tar.zst | tail -3
    export QT_QPA_PLATFORM=offscreen
    schneidi --version
    schneidi-cli --version
    schneidi-cli help >/dev/null
    ! ldd /usr/bin/schneidi /usr/bin/schneidi-cli | grep "not found"
    echo SMOKE_OK' 2>&1 | tee "$out/smoke.log"
grep -q SMOKE_OK "$out/smoke.log"

rm -rf "$work"
(cd "$out" && sha256sum "$(basename "$pkg")")
