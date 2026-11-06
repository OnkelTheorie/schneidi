// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Every media file the engine reads is opened through here: avformat_open_input limited to local files (no network or
// other protocols) and to demuxers that read only the file itself. Playlists, manifests and compositions (HLS, DASH,
// IMF, concat lists, SDP/RTP/RTSP, …) name further files or addresses inside the file and FFmpeg follows them, chosen
// by content whatever the file is called: a server's work folder (cli/Sandbox) would read anything through one.
// schneidi never needs them for media; image sequences (image2) and single files open as before.

#include <cstdint>

struct AVDictionary;
struct AVFormatContext;
struct AVIOContext;

namespace OpenMedia {

// The most read of a file to find its end when the header states no length (MediaProbe, Audio::Reader: the last 1, 8,
// then 64 MB, under a ReadLimit of this much in all). A recording of many GB whose tail holds no timed packet (zeros a
// crash left behind) is not read whole on import; such a file has no length then.
inline constexpr long long kTailScanMaxBytes = 64LL << 20;

// Reads of `fmt`'s file fail (AVERROR_EXIT) once `bytes` more were read, until this goes away (the file's own read function
// again then). A scan of a file's tail cannot bound itself by where it seeks: a demuxer may go back and
// read on from there - Matroska, after junk it cannot parse, resyncs from its last cluster, through the whole file.
// Does nothing for a context without its own I/O.
class ReadLimit {
public:
    ReadLimit(AVFormatContext* fmt, long long bytes);
    ~ReadLimit();
    ReadLimit(const ReadLimit&) = delete;
    ReadLimit& operator=(const ReadLimit&) = delete;
    bool reached() const { return m_left <= 0; }

private:
    // In place of the file's read function; finds its limit by the file's opaque (unchanged: FFmpeg hands it to other
    // functions of the file as well) among the limits of this thread
    static int read(void* opaque, uint8_t* buf, int size);
    AVIOContext* m_pb = nullptr;
    void* m_opaque = nullptr;
    int (*m_read)(void*, uint8_t*, int) = nullptr;
    long long m_left = 0;
};

// avformat_open_input(fmt, path, nullptr, options) with the limits; a refused demuxer fails it (AVERROR(EINVAL)), as
// does a path that exists but is no regular file (folder, named pipe, device, socket: see notAFile).
// `options`: further demuxer options (analyzeduration, probesize …), not changed
int input(AVFormatContext** fmt, const char* path, const AVDictionary* options = nullptr);
// Would input() refuse this demuxer? ("hls", "imf": yes; "mov,mp4,m4a,3gp,3g2,mj2": no)
bool refused(const char* demuxerName);
// Does `path` (UTF-8) exist but is no regular file? A named pipe would block the open, a device never end.
bool notAFile(const char* path);

} // namespace OpenMedia
