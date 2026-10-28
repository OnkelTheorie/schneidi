// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Own audio engine (docs/archiv/plan-0.10-ton.md), first block: reads one audio stream of a media file as float samples at
// a fixed output rate, with sample-exact random access. Uses the FFmpeg libraries directly, no MLT.
//
// Time: position 0 is the picture's first frame (the first key frame, like MLT's frame 0), or the first sample of a
// file without a picture. A sound stream that starts later than the picture begins at a positive position, one that
// starts earlier at a negative one. Silence before the stream, after it and in gaps of its timestamps.
//
// Packets are placed by their timestamps like a player: gaps become silence, overlaps are dropped, small jitter
// (Matroska's 1 ms ticks) is ignored. Jumps decode from a little earlier (pre-roll for AAC/Opus/MP3 and the
// resampler) on a fixed grid, so a position reads the same samples however it is reached.
//
// Not thread-safe: one reader per user. No GUI dependency (CLI/MCP use it too).

#include <QString>

#include <cstdint>
#include <memory>

namespace Audio {

struct StreamInfo {
    QString codec;          // FFmpeg decoder name
    int sourceRate = 0;     // sample rate of the decoded stream
    int sourceChannels = 0;
    int64_t start = 0;      // first sample of the stream (output samples, relative to position 0)
    int64_t end = 0;        // end of the stream as the container states it (exclusive, estimate)
    bool hasPicture = false; // position 0 = the picture's first frame
};

class Reader {
public:
    struct Options {
        int rate = 48000;
        int channels = 0; // 0 = like the source; otherwise remixed (FFmpeg's standard matrix)
    };

    // `stream` = n-th audio stream of the file (Clip::audioStream, 0 = first). nullptr if there is none.
    static std::unique_ptr<Reader> open(const QString& path, int stream, const Options& options, QString* error = nullptr);
    static std::unique_ptr<Reader> open(const QString& path, int stream = 0) { return open(path, stream, Options{}); }
    ~Reader();

    const StreamInfo& info() const;
    int rate() const;
    int channels() const;

    // Fills out[channel][0..count) with the samples [pos, pos + count). False after a read error (the rest silent).
    bool read(int64_t pos, int count, float* const* out);

    // Counters for tests and the log
    struct Stats {
        int seeks = 0;         // demuxer seeks (incl. retries)
        int gaps = 0;          // timestamp gaps filled with silence
        int outliers = 0;      // single packets far ahead of the stream, placed where the stream was
        int overlaps = 0;      // overlapping packets trimmed
        int decodeErrors = 0;
        int reopens = 0;       // jumps to the start that needed a fresh demuxer
    };
    const Stats& stats() const;

private:
    Reader();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace Audio
