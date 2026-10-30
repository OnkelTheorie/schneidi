#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU): one quad covering rect (target pixels); passes the target pixel position on
layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 dpos;
layout(std140, binding = 0) uniform Uniforms {
    vec4 rect;    // destination rectangle drawn, pixels of the target: x0, y0, x1, y1
    vec4 target;  // target width, height, NDC y sign (1 or -1), mode
    vec4 inv0;    // destination -> source pixel map: a, b, c (x), opacity
    vec4 inv1;    // d, e, f (y), mip level
    vec4 src;     // source width, height (of level 0), chroma subsampling log2 x, y
    vec4 chroma;  // chroma offset x, y (chroma texels), sample scale (16-bit planes), grade on (> 0.5)
    mat4 yuv;     // columns: R G B (A unused) = yuv * vec4(Y, U, V, 1)
    vec4 lut;     // 3D LUT: scale, offset; transition: progress, softness
    vec4 crop;    // source pixels kept: x0, y0, x1, y1 (x1 < x0 = no crop)
};
void main()
{
    dpos = mix(rect.xy, rect.zw, corner);
    gl_Position = vec4(dpos.x / target.x * 2.0 - 1.0, (dpos.y / target.y * 2.0 - 1.0) * target.z, 0.0, 1.0);
}
