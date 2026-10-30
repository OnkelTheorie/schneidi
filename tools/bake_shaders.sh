#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Bakes the GPU shaders into .qsb files for every Qt RHI backend: SPIR-V (Vulkan), GLSL, HLSL (Direct3D 11/12) and MSL
# (Metal). The .qsb files are in the repo, so a build needs no Qt Shader Tools; run this after changing a shader.
# Needs qsb (Debian: qt6-shader-baker).
# - src/ui/shaders: the viewer (GLSL 100 es/120/150: runs on the oldest OpenGL too)
# - src/engine/picture/shaders: the picture engine (texelFetch/textureSize: GLSL 300 es/150 and newer)
# Usage: tools/bake_shaders.sh [path to qsb]
set -e
QSB=${1:-qsb}
command -v "$QSB" >/dev/null 2>&1 || QSB=/usr/lib/qt6/bin/qsb
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root/src/ui/shaders"
for f in *.vert *.frag; do
    "$QSB" --glsl "100 es,120,150" --hlsl 50 --msl 12 -o "$f.qsb" "$f"
done
cd "$root/src/engine/picture/shaders"
for f in *.vert *.frag; do
    "$QSB" --glsl "300 es,150,330" --hlsl 50 --msl 12 -o "$f.qsb" "$f"
done
