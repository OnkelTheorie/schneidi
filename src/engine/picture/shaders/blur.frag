#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU), compositing node "blur": one box pass of VideoEffects::gaussian (three boxes per axis of the
// radii of matteBlurRadii, edges repeated), on premultiplied values (as the CPU does for pictures with transparency).
// P0 = direction x, y (1, 0 or 0, 1), radius. Input tex0, output RGBA16F premultiplied.
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
// The 10 parameter vec4 of the compositing passes live in the shared block after rect and target (Context::compPass)
#define P0 inv0
#define P1 inv1
#define P2 src
#define P3 chroma
#define P4 yuv[0]
#define P5 yuv[1]
#define P6 yuv[2]
#define P7 yuv[3]
#define P8 lut
#define P9 crop

// A premultiplied texel as the CPU holds the pixel: straight 8-bit values
ivec4 bytesOf(vec4 c)
{
    int a = int(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5);
    if (a <= 0) return ivec4(0);
    return ivec4(ivec3(clamp(c.rgb / c.a, 0.0, 1.0) * 255.0 + 0.5), a);
}
// Straight 8-bit values premultiplied again (what the target stores)
vec4 colorOf(ivec4 b)
{
    float a = float(b.a) / 255.0;
    return vec4(vec3(b.rgb) / 255.0 * a, a);
}
// Integer division truncating toward zero (as C++) for either sign
int divT(int x, int d) { return x >= 0 ? x / d : -((-x) / d); }

void main()
{
    ivec2 p = ivec2(floor(dpos));
    ivec2 dir = ivec2(int(P0.x + 0.5), int(P0.y + 0.5));
    int r = int(P0.z + 0.5);
    ivec2 last = textureSize(tex0, 0) - 1;
    vec4 sum = vec4(0.0);
    for (int k = -r; k <= r; ++k) sum += texelFetch(tex0, clamp(p + k * dir, ivec2(0), last), 0);
    fragColor = sum / float(2 * r + 1);
}
