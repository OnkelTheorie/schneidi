#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// FrameView: one quad (corners 0..1) placed at `rect` (normalized device coordinates x0, y0, x1, y1), texture
// coordinates from `uvRect` (u0, v0, u1, v1; 0,0 = top left of the frame)
layout(location = 0) in vec2 corner;
layout(location = 0) out vec2 uv;
layout(std140, binding = 0) uniform Placement {
    vec4 rect;
    vec4 uvRect;
};
void main()
{
    uv = mix(uvRect.xy, uvRect.zw, corner);
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}
