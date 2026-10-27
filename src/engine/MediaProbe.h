#pragma once
// What Engine::probe tells about a media file (length, picture, sound, audio streams), read with libavformat directly
// instead of opening an MLT producer: Matroska/WebM, MP4/MOV, WAV, FLAC, MP3 and AVI from their header alone (~1 ms;
// MLT took 0.1-0.5 s, a game recording with several sound streams the most, because it decodes the first frames of
// every stream), other containers with libavformat's stream analysis (what MLT runs too, once instead of per
// producer). Same results as the MLT producer (test_media_probe compares both on varied files).
// Not decided here (nullopt, Engine::probe asks MLT): still images and everything else MLT's loader does not hand to
// its avformat producer (loader.dict: images, .mlt/.xml, titles, …), files libavformat cannot open, an unknown
// length, streams without a decoder. Thread-safe (the Media Pool probes several files at once).

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QString>

#include <optional>

namespace MediaProbe {

// Lengths count in `rate`
std::optional<MediaInfo> read(const QString& path, FrameRate rate);
// Sound files by extension (mp3, wav, …): an embedded cover is no picture for the timeline
bool isAudioFile(const QString& path);

} // namespace MediaProbe
