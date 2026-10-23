#pragma once
// Which audio path is used (docs/archiv/plan-0.10-ton.md): the own engine SMT (Audio::Mixer, the default since 0.10) or the
// old one through MLT, kept as a fallback for one version.
//
// Chosen by the environment variable SCHNEIDI_AUDIO ("own" or "mlt"), otherwise the setting "audio/engine" (same
// values). Anything but "mlt" is the own engine.

namespace Audio {

bool ownPath();
// The setting "audio/engine" (the environment variable still wins); takes effect at the next start
void setOwnPath(bool own);

} // namespace Audio
