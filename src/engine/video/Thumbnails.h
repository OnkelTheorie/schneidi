#pragma once
// Small pictures of a video file without a process per file (timeline filmstrips, Media Storage previews): the FFmpeg
// libraries in the calling thread, software decoding, libswscale. The pictures are upright (rotation metadata), in
// square pixels and in the stream's colors (matrix and range as the preview reads them).
//
// keyFrames() feeds only the key frame packets to the decoder (the other packets are not even parsed): one picture per
// key frame, ~20 ms instead of ~150–700 ms for an exactly sought 4K frame. No GUI dependency (QImage only).

#include <QImage>
#include <QSize>
#include <QString>

#include <functional>

namespace Video {

struct KeyFrameOptions {
    QSize box;        // pictures fitted into this size, centered on black (the project frame's borders)
    int threads = 0;  // decoder threads (0 = FFmpeg's choice); independent key frames decode in parallel
};

// One picture per key frame of the first video stream (no cover pictures), in file order. onPicture gets the time in
// seconds from the file's start (as the ffmpeg command line counts it: the container's start time is 0) and the
// picture (Format_RGB32, `box` large); returning false stops. Returns the number of pictures, -1 if the file has no
// readable video stream.
int keyFrames(const QString& path, const KeyFrameOptions& options, const std::function<bool(double, const QImage&)>& onPicture);

// The picture shown at `seconds` (exact: decoded from the key frame before), fitted into `box` without borders; the
// first picture if the file is shorter. Null if the file has no readable video.
QImage stillAt(const QString& path, double seconds, const QSize& box);

} // namespace Video
