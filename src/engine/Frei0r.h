#pragma once
// frei0r plugins as effects (Effects Library: Open FX → frei0r). At startup every frei0r filter MLT knows is read from
// the MLT metadata and registered in the EffectRegistry (id = MLT service, e.g. "frei0r.glow"); the Inspector builds
// its controls from the parameters (number, checkbox, color, choice). Number parameters are animatable (keyframes
// via Keys::effectParam, rendered as MLT animation by the TimelineBuilder). Besides the system or bundled plugins, MLT also
// loads the ones in the effects folder (EffectFolders::frei0rDir, picked up after a restart).

namespace Frei0r {

// Append the effects folder to FREI0R_PATH (after Bundle::prepareMltEnvironment, before Mlt::Factory::init)
void prepareEnvironment();
// After Mlt::Factory::init: register the filters in the EffectRegistry; returns how many
int registerEffects();

} // namespace Frei0r
