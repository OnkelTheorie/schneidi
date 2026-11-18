#!/usr/bin/env bash
# Builds the release AppImage in the Ubuntu 22.04 container (packaging/linux/Dockerfile.appimage), so it runs on
# glibc 2.35 / GLIBCXX_3.4.30 and newer. Arguments and environment like build-appimage.sh (GPL_ENCODERS, JOBS).
# Run from the project folder: packaging/linux/build-appimage-docker.sh  ->  dist/schneidi-<version>-x86_64.AppImage
# The container's build folders (build-appimage*, build-ffmpeg*) are its own: a host build of the same folder (other
# compiler, other Qt) is not reused, so they live under build-docker/.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${IMAGE:-schneidi-appimage-2204}"
docker build -f "$ROOT/packaging/linux/Dockerfile.appimage" -t "$IMAGE" "$ROOT/packaging/linux"
mkdir -p "$ROOT/build-docker/home"
# FFmpeg's work folder (download, sources, logs) next to the container's prefix, apart from a host build
if [ "${GPL_ENCODERS:-1}" = 1 ]; then FFMPEG_WORK=/w/build-docker/build-ffmpeg; else FFMPEG_WORK=/w/build-docker/build-ffmpeg-lgpl; fi
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/w" -w /w -e HOME=/w/build-docker/home \
    -e GPL_ENCODERS="${GPL_ENCODERS:-1}" -e JOBS="${JOBS:-$(nproc)}" -e CMAKE_BUILD_PARALLEL_LEVEL="${JOBS:-$(nproc)}" \
    -e APPIMAGE_BUILD_ROOT=/w/build-docker -e FFMPEG_WORK="$FFMPEG_WORK" -e ABI_LIMITS="--glibc 2.35 --glibcxx 3.4.30 --cxxabi 1.3.13" \
    "$IMAGE" packaging/linux/build-appimage.sh "$@"
