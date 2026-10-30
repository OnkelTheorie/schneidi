#version 440
// SPDX-License-Identifier: AGPL-3.0-only
// FrameView: the frame's texel, blended over the letterbox colour with its (straight) alpha by the pipeline
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D frame;
void main()
{
    fragColor = texture(frame, uv);
}
