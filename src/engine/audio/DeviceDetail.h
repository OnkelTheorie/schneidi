// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Helpers shared by the output (Device.cpp) and the input (Capture.cpp) of the own audio engine; only for those two.

#include "engine/audio/Device.h"
#include "engine/audio/Miniaudio.h"

#include <utility>
#include <vector>

namespace Audio {

// "DirectSound" == "directsound", "Core Audio" == "coreaudio"
QString plainName(const QString& name);
// A miniaudio context of the chosen backend ("" = miniaudio's order)
bool initContext(const QString& backend, ma_context* context);
// Periods of the device for `buffer` samples. PulseAudio (PipeWire's pulse server) runs the whole sound graph at a
// quarter of miniaudio's tlength (one period): 2 x 512 forced a 128-sample quantum on every program - Spotify and the
// user's filter-chain mixer got xruns, everything crackled (20.11.). There: one period of the whole buffer, at least
// 2048 (quantum >= 512; the default 1024 from 4096 on). Elsewhere two periods of half the buffer.
void setBuffer(ma_device_config* config, const ma_context* context, int buffer);
// The id of a device as text that stays the same across restarts (the setting keeps it)
QString idText(ma_backend backend, const ma_device_id& id);
// Playback or capture devices of an open context
std::vector<std::pair<Device::Output, ma_device_id>> listDevices(ma_context* context, bool capture);

} // namespace Audio
