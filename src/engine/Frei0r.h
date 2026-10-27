#pragma once
// frei0r plugins as effects (Effects Library: Open FX → frei0r), opt-in: off by default, schneidi's own effects
// (engine/VideoEffects) replace the plugins it used to offer and schneidi does not bundle frei0r. When it is off,
// MLT gets an empty FREI0R_PATH and loads no plugin at all. When on, at startup every frei0r filter MLT knows is read from
// the MLT metadata and registered in the EffectRegistry (id = MLT service, e.g. "frei0r.glow"); the Inspector builds
// its controls from the parameters (number, checkbox, color, choice). Number parameters are animatable (keyframes
// via Keys::effectParam, rendered as MLT animation by the TimelineBuilder). Besides the system or bundled plugins, MLT also
// loads the ones in the effects folder (EffectFolders::frei0rDir, picked up after a restart).

namespace Frei0r {

// The setting (QSettings "effects/frei0r", Settings → General; takes effect after a restart). SCHNEIDI_FREI0R=1 in
// the environment also switches it on (tests).
bool enabled();
void setEnabled(bool on);

// Enabled: append the effects folder to FREI0R_PATH; off: point it at nothing (after Bundle::prepareMltEnvironment,
// before Mlt::Factory::init)
void prepareEnvironment();
// After Mlt::Factory::init: register the filters in the EffectRegistry (not the ones schneidi has its own effect
// for); returns how many. Off: none
int registerEffects();

} // namespace Frei0r
