#pragma once
// Which audio path is used (docs/plan-0.10-ton.md): the own engine (Audio::Mixer) or the old one through MLT. The
// MLT path stays as a fallback until the own engine is at least as good everywhere, then it goes.
//
// Chosen by the environment variable SCHNEIDI_AUDIO ("own" or "mlt"), otherwise the setting "audio/engine" (same
// values). Default: MLT until the switch-over (step 6 of the plan).

namespace Audio {

bool ownPath();
// The setting "audio/engine" (the environment variable still wins); takes effect at the next start
void setOwnPath(bool own);

} // namespace Audio
