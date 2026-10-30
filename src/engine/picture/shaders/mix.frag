#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// Picture engine (GPU): transitions between two pictures of the target's size (premultiplied), written without
// blending. target.w (mode): 0 = cross dissolve, a*(1-t) + b*t in premultiplied alpha (Renderer mix());
// 1 = luma wipe: b replaces a where the gradient (tex2 .r) is below progress*(1+softness), smoothstep over the
// softness band, mixed in straight alpha (Renderer luma())
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

vec4 straight(vec4 c) { return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0); }

void main()
{
    ivec2 p = ivec2(floor(dpos));
    vec4 a = texelFetch(tex0, p, 0), b = texelFetch(tex1, p, 0);
    float t = lut.z;
    if (target.w < 0.5) {
        fragColor = mix(a, b, t);
        return;
    }
    float v = texelFetch(tex2, p, 0).r;
    float soft = lut.w, weight = t * (1.0 + soft);
    float k = weight < v ? 0.0 : (weight >= v + soft ? 1.0 : smoothstep(0.0, 1.0, (weight - v) / soft));
    vec4 s = mix(straight(a), straight(b), k);
    fragColor = vec4(s.rgb * s.a, s.a);
}
