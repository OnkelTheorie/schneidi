#pragma once
// Own MLT "volume" filter: MLT's (normalize module) is GPL-2+ and is not loaded any more (0.14: no third-party GPL
// code). Same behaviour for what schneidi uses: property "level" in dB (animatable, -200 = silence), float audio,
// gain ramped linearly over a frame from the previous frame's gain when the frames follow each other (no clicks on
// keyframes and fades). Not supported: "gain", "normalize", "max_gain", "limiter" (schneidi never sets them).

namespace Volume {

// Registers "schneidi_volume" and, when no other module provides it, "volume" (old graphs, presets, the TimelineBuilder)
void registerService();

} // namespace Volume
