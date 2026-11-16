#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU): draws one source through an affine or projective map onto the target, like Compose::draw (bilinear in
// premultiplied alpha, texels outside the source transparent, mip level chosen by the caller for 2:1 and more, a grid
// of taps for what is still 2:1 or more at that level),
// optionally converting Y'CbCr planes and applying the colour grade (baked 3D LUT) on the way. Output premultiplied;
// the pipeline blends it "over" (One, OneMinusSrcAlpha).
// target.w (mode): 0 = RGBA premultiplied, 1 = RGBA straight, 2 = planar Y'CbCr (tex0 Y, tex1 Cb, tex2 Cr),
// 3 = semi-planar (tex0 Y, tex1 CbCr in .rg)
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
    vec4 lut;     // 3D LUT: scale, offset; source texels of the mip level per target pixel along source x, y
    vec4 crop;    // source pixels kept: x0, y0, x1, y1 (x1 < x0 = no crop)
    vec4 persp;   // destination -> source w = dot(persp.xyz, (x, y, 1)) (projective; affine: 0, 0, 1)
    vec4 tone;    // HDR -> SDR of Y'CbCr: transfer (0 none, 1 HLG, 2 PQ), BT.2020 primaries (1) or BT.709 (0)
};
layout(binding = 1) uniform sampler2D tex0;
layout(binding = 2) uniform sampler2D tex1;
layout(binding = 3) uniform sampler2D tex2;
layout(binding = 4) uniform sampler3D lut3;

// Texel p of tex0 premultiplied, transparent outside
vec4 rgbaTexel(ivec2 p, ivec2 size, int lod, bool straight)
{
    if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) return vec4(0.0);
    vec4 c = texelFetch(tex0, p, lod);
    if (straight) c.rgb *= c.a;
    return c;
}

// Bilinear in a plane at texel-centre-relative position q, clamped at the edges
vec4 planeBilinear(int which, vec2 q, int lod)
{
    ivec2 size = which == 1 ? textureSize(tex1, lod) : textureSize(tex2, lod);
    vec2 f = fract(q);
    ivec2 i = ivec2(floor(q));
    ivec2 i0 = clamp(i, ivec2(0), size - 1), i1 = clamp(i + 1, ivec2(0), size - 1);
    vec4 a, b, c, d;
    if (which == 1) {
        a = texelFetch(tex1, ivec2(i0.x, i0.y), lod); b = texelFetch(tex1, ivec2(i1.x, i0.y), lod);
        c = texelFetch(tex1, ivec2(i0.x, i1.y), lod); d = texelFetch(tex1, ivec2(i1.x, i1.y), lod);
    } else {
        a = texelFetch(tex2, ivec2(i0.x, i0.y), lod); b = texelFetch(tex2, ivec2(i1.x, i0.y), lod);
        c = texelFetch(tex2, ivec2(i0.x, i1.y), lod); d = texelFetch(tex2, ivec2(i1.x, i1.y), lod);
    }
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// One bilinear sample at texel-centre-relative position q of the mip level: premultiplied colour (RGBA modes,
// graded later) or, for Y'CbCr, vec4(Y, Cb, Cr, share of the taps inside the picture) with Y'CbCr times that share
vec4 sampleAt(vec2 q, ivec2 size, int lod, int mode)
{
    ivec2 i = ivec2(floor(q));
    vec2 f = q - vec2(i);
    vec4 w = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    if (mode <= 1) {
        bool straight = mode == 1;
        return w.x * rgbaTexel(i, size, lod, straight) + w.y * rgbaTexel(i + ivec2(1, 0), size, lod, straight)
             + w.z * rgbaTexel(i + ivec2(0, 1), size, lod, straight) + w.w * rgbaTexel(i + ivec2(1, 1), size, lod, straight);
    }
    // Luma: the taps inside the picture, weighted (alpha = their share)
    float ysum = 0.0, alpha = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 p = i + ivec2(k & 1, k >> 1);
        if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) continue;
        ysum += w[k] * texelFetch(tex0, p, lod).r;
        alpha += w[k];
    }
    if (alpha <= 0.0) return vec4(0.0);
    // Chroma at its own grid (subsampled, sited by chroma.xy)
    vec2 cq = (q + 0.5) / exp2(src.zw) - 0.5 + chroma.xy;
    vec2 uv;
    if (mode == 2) uv = vec2(planeBilinear(1, cq, lod).r, planeBilinear(2, cq, lod).r);
    else uv = planeBilinear(1, cq, lod).rg;
    return vec4(ysum, uv * alpha, alpha);
}

// HDR -> SDR, the same calculation as engine/HdrTone (keep both alike): signal R'G'B' -> relative display light
// (HLG: inverse OETF + OOTF for 1000 nits, PQ: EOTF; 1 = reference white 203 nits) -> BT.709 primaries -> tone curve on
// the luminance -> out-of-gamut colours towards their luminance -> BT.1886 inverse
vec3 hdrToSdr(vec3 c)
{
    vec3 lin;
    bool wide = tone.y > 0.5;
    if (tone.x > 1.5) { // PQ
        vec3 p = pow(c, vec3(1.0 / 78.84375));
        lin = 10000.0 / 203.0 * pow(max(p - 0.8359375, vec3(0.0)) / (18.8515625 - 18.6875 * p), vec3(1.0 / 0.1593017578125));
    } else { // HLG
        vec3 e = mix(c * c / 3.0, (exp((c - 0.55991073) / 0.17883277) + 0.28466892) / 12.0, greaterThan(c, vec3(0.5)));
        float ys = dot(wide ? vec3(0.2627, 0.6780, 0.0593) : vec3(0.2126, 0.7152, 0.0722), e);
        lin = ys > 0.0 ? e * (1000.0 / 203.0 * pow(ys, 0.2)) : vec3(0.0);
    }
    if (wide)
        lin = vec3(dot(vec3(1.660491, -0.587641, -0.072850), lin), dot(vec3(-0.124550, 1.132900, -0.008349), lin),
                   dot(vec3(-0.018151, -0.100579, 1.118730), lin));
    lin *= 0.6; // HdrTone kGain
    float y = dot(vec3(0.2126, 0.7152, 0.0722), lin);
    if (y <= 0.0) return vec3(0.0);
    const float knee = 0.45, peak = 1000.0 / 203.0 * 0.6, slope = (peak - knee) / (1.0 - knee);
    float x = (y - knee) / (peak - knee);
    float m = y <= knee ? y : knee + (1.0 - knee) * (x * slope / (1.0 + (slope - 1.0) * x));
    if (m >= 1.0) return vec3(1.0);
    lin *= m / y;
    float lo = min(lin.r, min(lin.g, lin.b));
    if (lo < 0.0) lin = m + (lin - m) * (m / (m - lo));
    float hi = max(lin.r, max(lin.g, lin.b));
    if (hi > 1.0) lin = m + (lin - m) * ((1.0 - m) / (hi - m));
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.4));
}

// Share of source position st (pixels of level 0) kept by the crop (area share like Compose::crop; 1 = no crop)
float cropShare(vec2 st)
{
    if (crop.z < crop.x) return 1.0;
    vec2 lo = clamp(st - crop.xy + 0.5, 0.0, 1.0), hi = clamp(crop.zw - st + 0.5, 0.0, 1.0);
    return lo.x * lo.y * hi.x * hi.y;
}

void main()
{
    int mode = int(target.w + 0.5);
    int lod = int(inv1.w + 0.5);
    float scale = exp2(-float(lod));
    // Source position of this pixel's centre (dpos is the centre already), in texels of the mip level
    float w = dot(persp.xyz, vec3(dpos, 1.0));
    if (w <= 1e-6) discard; // beyond the horizon of a picture tilted in depth
    vec2 s = vec2(dot(inv0.xyz, vec3(dpos, 1.0)), dot(inv1.xyz, vec3(dpos, 1.0))) / w;
    vec2 q = s * scale - 0.5;
    ivec2 size = textureSize(tex0, lod);
    // Still 2:1 or more along a source axis at this level (no mips, or squeezed more along one axis than the level
    // covers): average a grid of bilinear taps over the pixel's footprint there, as Compose::draw halves before its
    // bilinear step - bilinear alone would skip texels (stair-steps, moire). lut.zw: source texels of the level per
    // target pixel along the source's x and y (below 2: one tap)
    vec2 foot = max(lut.zw, vec2(1.0));
    ivec2 taps = clamp(ivec2(floor(foot)), ivec2(1), ivec2(16));
    vec2 reach = 0.5 * foot * vec2(greaterThan(taps, ivec2(1)));
    if (q.x < -1.0 - reach.x || q.y < -1.0 - reach.y || q.x >= float(size.x) + reach.x || q.y >= float(size.y) + reach.y)
        discard;
    vec4 acc;
    if (taps.x == 1 && taps.y == 1) { // the usual case (same for every pixel of the draw: no divergence)
        acc = sampleAt(q, size, lod, mode) * cropShare(s);
    } else {
        // Each tap cropped on its own: once at the pixel's centre, a pixel at the crop edge took in the texels cut away
        acc = vec4(0.0);
        for (int ty = 0; ty < taps.y; ++ty)
            for (int tx = 0; tx < taps.x; ++tx) {
                vec2 qt = q + ((vec2(tx, ty) + 0.5) / vec2(taps) - 0.5) * 2.0 * reach;
                acc += sampleAt(qt, size, lod, mode) * cropShare((qt + 0.5) / scale);
            }
        acc /= float(taps.x * taps.y);
    }
    vec4 color;
    if (mode <= 1) {
        color = acc;
        if (chroma.w > 0.5 && color.a > 0.0) { // grade on straight colour
            vec3 c = clamp(color.rgb / color.a, 0.0, 1.0);
            color.rgb = texture(lut3, c * lut.x + lut.y).rgb * color.a;
        }
    } else {
        float alpha = acc.a;
        if (alpha <= 0.0) discard;
        float Y = acc.r / alpha * chroma.z;
        vec2 uv = acc.gb / alpha * chroma.z;
        vec3 c = clamp((yuv * vec4(Y, uv, 1.0)).rgb, 0.0, 1.0);
        if (tone.x > 0.5) c = hdrToSdr(c);
        if (chroma.w > 0.5) c = texture(lut3, c * lut.x + lut.y).rgb;
        color = vec4(c * alpha, alpha);
    }
    fragColor = color * inv0.w;
}
