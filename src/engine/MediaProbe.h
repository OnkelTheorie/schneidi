// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// What Engine::probe tells about a media file (length, picture, sound, audio streams), read with libavformat:
// Matroska/WebM, MP4/MOV, WAV, FLAC, MP3 and AVI from their header alone (~1 ms), other containers with libavformat's
// stream analysis. Still pictures when Qt can read them: from the picture's header (QImageReader::canRead), 5 seconds
// long. Not readable (nullopt, Engine::probe reports no picture and no sound): project files of other editors,
// titles, animations, unreadable pictures, files libavformat cannot open, an unknown length, streams without a
// decoder. Thread-safe (the Media Pool probes several files at once).

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
