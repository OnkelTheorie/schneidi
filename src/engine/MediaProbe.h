// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// What Engine::probe tells about a media file (length, picture, sound, audio streams), read with libavformat:
// Matroska/WebM, MP4/MOV, WAV, FLAC, MP3 and AVI from their header alone (~1 ms), other containers with libavformat's
// stream analysis. Still pictures when Qt can read them: from the picture's header (QImageReader::canRead), 5 seconds
// long. Not readable (nullopt, Engine::probe reports no picture and no sound): project files of other editors,
// titles, animations, unreadable pictures, files libavformat cannot open, an unknown length, no picture or sound
// stream with a decoder. A stream that cannot be played (no decoder here, values the stream analysis did not find)
// does not refuse the others: the picture and the sound that can be played are reported (as the preview's reader
// takes the picture alone). Thread-safe (the Media Pool probes several files at once).

#include "core/ProjectFormat.h"
#include "core/SourceBitrate.h"
#include "core/Types.h"

#include <QString>

#include <optional>

namespace MediaProbe {

struct ReadOptions {
    // A stream the stream analysis found no values for (no packet in the first 5 s) gets a second analysis of up to
    // 60 s / 120 MB (once per file while it stays the same: remembered whether it helped). false = a quick look that
    // never does (on the UI thread: a drop preview); the result is then partial and says so in `partial`.
    bool longAnalysis = true;
    bool* partial = nullptr;
};
// Lengths count in `rate`
std::optional<MediaInfo> read(const QString& path, FrameRate rate, const ReadOptions& options = {});
// Why read() found nothing to play in `path`, in a short English sentence for a message (`cannot read media: <path>
// (<reason>)`): a folder, a pipe or device, no permission, an empty file, no picture or sound streams, a still picture
// that is broken or too large, … Only meant for a file read() refused (it opens the file again).
QString whyUnreadable(const QString& path);
// Might `path` be unreadable only for now? Locked: another program holds it so it cannot be opened (Windows: a
// recorder, a copy or a virus scan that shares no read access). Written: it was written to moments ago (a recording,
// download or copy may still be in progress). A message then offers to try again.
enum class Busy { No, Written, Locked };
Busy busy(const QString& path);
// Tests: how often a file was opened a second time for the long stream analysis; forget what it found per file
int longAnalysesForTest();
void forgetForTest();
// Sound files by extension (mp3, wav, …): an embedded cover is no picture for the timeline
bool isAudioFile(const QString& path);

// The video stream's bit rate, picture size and frame rate (export quality "like source", core/SourceBitrate): the
// stream's bit rate from the container (MP4/MOV: its samples' sizes; Matroska: a "BPS" statistics tag), else the
// file's average bit rate (size / length) minus its sound streams'. nullopt: no video, a still picture or cover, or no
// bit rate known. From the header where it suffices (~1 ms), remembered per file while its size and time stay the
// same. Thread-safe.
std::optional<SourceBitrate::Video> videoRate(const QString& path);

} // namespace MediaProbe
