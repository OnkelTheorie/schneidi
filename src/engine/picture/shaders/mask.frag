#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU), compositing node "mask" (CompNodesMask evalMask + engine/MaskRaster): rect/ellipse as the
// Color page window (GradeWindows::Mask), linear/radial gradients; invert; combined with the mask at tex0.
// P0 = shape (0 rect, 1 ellipse, 2 linear, 3 radial), invert, combine (-1 none, 0 add, 1 subtract, 2 intersect)
// P1 = centre x, y, half width, half height (pixels); P2 = cos, sin, soft; P3 = pixels drawn x0, y0, x1, y1 (inclusive)
// P4 = gradient a.x, a.y, b.x, b.y. Output: the 8-bit value / 255 in every channel.
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

int shapeValue(ivec2 p)
{
    if (p.x < int(P3.x) || p.y < int(P3.y) || p.x > int(P3.z) || p.y > int(P3.w)) return 0;
    int shape = int(P0.x + 0.5);
    if (shape >= 2) {
        vec2 a = P4.xy, d = P4.zw - P4.xy;
        vec2 q = vec2(p) + 0.5 - a;
        float v;
        if (shape == 2) v = 1.0 - dot(q, d) / dot(d, d);
        else v = 1.0 - length(q) / length(d);
        return v <= 0.0 ? 0 : (v >= 1.0 ? 255 : int(v * 255.0 + 0.5));
    }
    vec2 dd = vec2(p) + 0.5 - P1.xy;
    float lx = dd.x * P2.x - dd.y * P2.y, ly = dd.x * P2.y + dd.y * P2.x;
    float hw = P1.z, hh = P1.w, soft = P2.z, dist;
    if (shape == 0) {
        float ox = abs(lx) - hw, oy = abs(ly) - hh;
        if (ox <= 0.0 && oy <= 0.0) return 255;
        dist = length(vec2(max(ox, 0.0), max(oy, 0.0)));
    } else {
        float ux = lx / hw, uy = ly / hh;
        float r = sqrt(ux * ux + uy * uy);
        if (r <= 1.0) return 255;
        float gx = ux / hw, gy = uy / hh;
        float grad = sqrt(gx * gx + gy * gy) / r;
        dist = grad > 0.0 ? (r - 1.0) / grad : 0.0;
    }
    if (dist >= soft) return 0;
    float f = 1.0 - dist / soft;
    float w = f * f * (3.0 - 2.0 * f);
    return w >= 1.0 ? 255 : int(w * 255.0 + 0.5);
}

void main()
{
    ivec2 p = ivec2(floor(dpos));
    int v = shapeValue(p);
    if (P0.y > 0.5) v = 255 - v;
    int combine = int(floor(P0.z + 0.5));
    if (combine >= 0) {
        int m = int(clamp(texelFetch(tex0, p, 0).a, 0.0, 1.0) * 255.0 + 0.5);
        if (combine == 0) v = max(m, v);
        else if (combine == 1) v = min(m, 255 - v);
        else v = min(m, v);
    }
    fragColor = vec4(float(v) / 255.0);
}
