// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Every media file the engine reads is opened through here: avformat_open_input limited to local files (no network or
// other protocols) and to demuxers that read only the file itself. Playlists, manifests and compositions (HLS, DASH,
// IMF, concat lists, SDP/RTP/RTSP, …) name further files or addresses inside the file and FFmpeg follows them, chosen
// by content whatever the file is called: a server's work folder (cli/Sandbox) would read anything through one.
// schneidi never needs them for media; image sequences (image2) and single files open as before.

struct AVDictionary;
struct AVFormatContext;

namespace OpenMedia {

// avformat_open_input(fmt, path, nullptr, options) with the limits; a refused demuxer fails it (AVERROR(EINVAL)).
// `options`: further demuxer options (analyzeduration, probesize …), not changed
int input(AVFormatContext** fmt, const char* path, const AVDictionary* options = nullptr);
// Would input() refuse this demuxer? ("hls", "imf": yes; "mov,mp4,m4a,3gp,3g2,mj2": no)
bool refused(const char* demuxerName);

} // namespace OpenMedia
