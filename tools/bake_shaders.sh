#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Bakes the viewer's shaders (src/ui/shaders/*.vert, *.frag) into .qsb files for every Qt RHI backend: SPIR-V (Vulkan),
# GLSL (OpenGL 2.1/3.2 core, GLES 2/3), HLSL (Direct3D 11/12) and MSL (Metal). The .qsb files are in the repo, so a
# build needs no Qt Shader Tools; run this after changing a shader. Needs qsb (Debian: qt6-shader-baker).
# Usage: tools/bake_shaders.sh [path to qsb]
set -e
QSB=${1:-qsb}
command -v "$QSB" >/dev/null 2>&1 || QSB=/usr/lib/qt6/bin/qsb
cd "$(dirname "$0")/../src/ui/shaders"
for f in *.vert *.frag; do
    "$QSB" --glsl "100 es,120,150" --hlsl 50 --msl 12 -o "$f.qsb" "$f"
done
