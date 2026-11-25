// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Deinterlacing of single pictures (Video::Reader, Options::deinterlace): FFmpeg's bwdif (yadif when the libraries
// lack it) with one picture per frame (send_frame), as the proxies of interlaced files are made
// (engine/ProxyManager.cpp), so the proxy, the preview of the original and the export show the same picture.
//
// Stateless on purpose: every call builds a small filter graph and feeds it the picture before, the picture itself and
// the picture after (the temporal neighbours bwdif looks at), and keeps only the picture itself. The result for frame N
// depends on frames N-1, N, N+1 only - never on the way the reader got there (played to it, seeked to it, a worker out
// of order) - which a running filter fed in decoding order cannot promise after a seek. The cost: a graph per picture
// (well below a millisecond next to the filter itself) and the neighbours decoded, which playback decodes anyway.

extern "C" {
struct AVFrame;
}

namespace Video {

// Is a deinterlacer there (libavfilter with bwdif or yadif)?
bool canDeinterlace();

// `cur` made progressive: the field shown first (`topFieldFirst`) is kept, the other one interpolated from both fields
// of `cur` and the fields of `prev` and `next` (the pictures before and after it; either may be `cur` itself: the start
// or end of a file, or a neighbour of another size or format). A new frame (the caller frees it), the properties of
// `cur`, flagged progressive; nullptr when no deinterlacer is there or the filter does not take the pixel format.
// Thread-safe (one graph per call, one thread each: frames render in parallel already).
AVFrame* deinterlace(const AVFrame* prev, const AVFrame* cur, const AVFrame* next, bool topFieldFirst);

} // namespace Video
