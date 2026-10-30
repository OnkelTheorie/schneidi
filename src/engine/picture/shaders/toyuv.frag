#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU): output conversion for the encoder (like OutputColor::fromRgba, 8 bit). The target is RGBA8
// with several code values packed into one texel, so a readback returns the plane bytes directly:
// target.w (mode) 0 = Y (texel x covers pixels 4x..4x+3), 1 = NV12 CbCr (2 pairs: pixels 4x..4x+3, rows 2y, 2y+1),
// 2 = Cb plane of 4:2:0 (4 values: pixels 8x..8x+7, rows 2y, 2y+1), 3 = Cr plane of 4:2:0.
// yuv columns 0..2: Y, Cb, Cr = dot(column.rgb, rgb) + column.a (all in 0..1 code value / 255)
layout(location = 0) in vec2 dpos;
layout(location = 0) out vec4 fragColor;
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
layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;
layout(binding = 3) uniform sampler2D tex2;
layout(binding = 4) uniform sampler3D lut3;

vec3 rgbAt(int x, int y)
{
    ivec2 size = textureSize(tex0, 0);
    vec4 c = texelFetch(tex0, clamp(ivec2(x, y), ivec2(0), size - 1), 0);
    return c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
}

vec3 avg2x2(int x, int y) { return (rgbAt(x, y) + rgbAt(x + 1, y) + rgbAt(x, y + 1) + rgbAt(x + 1, y + 1)) * 0.25; }

float code(int k, vec3 c) { return clamp(dot(yuv[k].rgb, c) + yuv[k].a, 0.0, 1.0); }

void main()
{
    int mode = int(target.w + 0.5);
    ivec2 t = ivec2(floor(dpos));
    vec4 o;
    if (mode == 0) {
        for (int k = 0; k < 4; ++k) o[k] = code(0, rgbAt(4 * t.x + k, t.y));
    } else if (mode == 1) {
        vec3 c0 = avg2x2(4 * t.x, 2 * t.y), c1 = avg2x2(4 * t.x + 2, 2 * t.y);
        o = vec4(code(1, c0), code(2, c0), code(1, c1), code(2, c1));
    } else {
        int which = mode == 2 ? 1 : 2;
        for (int k = 0; k < 4; ++k) o[k] = code(which, avg2x2(8 * t.x + 2 * k, 2 * t.y));
    }
    fragColor = o;
}
