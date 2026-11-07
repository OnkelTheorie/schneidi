#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU), compositing node "merge" (CompNodesCore evalMerge, the same integer steps on the same 8-bit
// values): fg (tex1) over bg (tex0) with a blend mode (engine/video/Blend.h index), fg alpha times opacity times the
// mask (tex2, its alpha). Kind 1 = an effect only where the mask is (mixByMask): tex0 = source, tex1 = effect result.
// P0 = Blend mode, opacity in 1/256, has bg, has fg; P1 = has mask, kind. Targets RGBA16F premultiplied.
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

float channel(int mode, float a, float b)
{
    if (mode == 1) return min(1.0, a + b);                                   // add
    if (mode == 3) return a * b;                                             // multiply
    if (mode == 4) return a + b - a * b;                                     // screen
    if (mode == 5) return a <= 0.5 ? 2.0 * a * b : 1.0 - 2.0 * (1.0 - a) * (1.0 - b); // overlay
    if (mode == 6) return min(a, b);                                         // darken
    if (mode == 7) return max(a, b);                                         // lighten
    if (mode == 12) return abs(a - b);                                       // difference
    return b;                                                                // normal
}

void main()
{
    ivec2 p = ivec2(floor(dpos));
    int mode = int(P0.x + 0.5), op = int(P0.y + 0.5);
    bool hasBg = P0.z > 0.5, hasFg = P0.w > 0.5, hasMask = P1.x > 0.5;
    vec4 bgTexel = hasBg ? texelFetch(tex0, p, 0) : vec4(0.0);
    if (P1.y > 0.5) { // mix by mask
        int k = int(clamp(texelFetch(tex2, p, 0).a, 0.0, 1.0) * 255.0 + 0.5);
        vec4 fx = texelFetch(tex1, p, 0);
        if (k >= 255) { fragColor = fx; return; }
        if (k <= 0) { fragColor = bgTexel; return; }
        ivec4 s = bytesOf(bgTexel), r = bytesOf(fx);
        fragColor = colorOf(s + ivec4(divT((r.r - s.r) * k, 255), divT((r.g - s.g) * k, 255), divT((r.b - s.b) * k, 255),
                                      divT((r.a - s.a) * k, 255)));
        return;
    }
    if (!hasFg) { fragColor = bgTexel; return; }
    ivec4 s = bytesOf(texelFetch(tex1, p, 0));
    ivec4 d = bytesOf(bgTexel);
    int sa = s.a;
    if (op != 256) sa = (sa * op + 128) >> 8;
    if (hasMask) sa = (sa * int(clamp(texelFetch(tex2, p, 0).a, 0.0, 1.0) * 255.0 + 0.5) + 127) / 255;
    if (sa == 0) { fragColor = bgTexel; return; }
    if (mode == 0 && !hasMask) { // Compose::over
        if (sa == 255) { fragColor = colorOf(ivec4(s.rgb, 255)); return; }
        int da = d.a;
        if (da == 255) {
            fragColor = colorOf(ivec4((s.rgb * sa + d.rgb * (255 - sa) + 127) / 255, 255));
            return;
        }
        int oa = sa * 255 + da * (255 - sa);
        fragColor = colorOf(ivec4((s.rgb * sa * 255 + d.rgb * da * (255 - sa) + oa / 2) / oa, (oa + 127) / 255));
        return;
    }
    if (d.a == 255) { // separable mode on an opaque backdrop: integer steps as the CPU table
        ivec3 o;
        for (int c = 0; c < 3; ++c) {
            int cb = d[c];
            int cm = int(clamp(channel(mode, float(cb) / 255.0, float(s[c]) / 255.0), 0.0, 1.0) * 255.0 + 0.5);
            o[c] = cb + divT((cm - cb) * sa + (cm >= cb ? 127 : -127), 255);
        }
        fragColor = colorOf(ivec4(o, 255));
        return;
    }
    float as = float(sa) / 255.0, ab = float(d.a) / 255.0;
    float ao = as + ab * (1.0 - as);
    ivec3 o;
    for (int c = 0; c < 3; ++c) {
        float cb = float(d[c]) / 255.0, cs = float(s[c]) / 255.0;
        float cm = (1.0 - ab) * cs + ab * channel(mode, cb, cs);
        o[c] = int(clamp((as * cm + (1.0 - as) * ab * cb) / ao, 0.0, 1.0) * 255.0 + 0.5);
    }
    fragColor = colorOf(ivec4(o, int(ao * 255.0 + 0.5)));
}
