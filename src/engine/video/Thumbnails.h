// SPDX-License-Identifier: AGPL-3.0-only
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
#include <vector>

struct AVFrame;

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

// Pictures at fixed spots, for analyses that must give the same result every time (black bars): the same file always
// gives the same pictures, whatever the machine's load (no decoder racing ahead decides which picture answers).
// `at` = spots as fractions of the duration (ascending). Each spot gets the key frame at or before it: a seek and one
// packet decoded. When fewer than `minKeys` different key frames answer the spots (a short file with a single key
// frame), each spot gets its exact picture instead, decoded from the key frame before (at most `maxDecode` pictures
// per spot; the one it got to then). Same key frame twice: given once. A file of one picture (or without a duration)
// gives its first picture only. onFrame gets the decoded frame (as stored: software format, not turned) and the quarter
// turns clockwise that show it upright; returning false stops. Returns the number of pictures given, -1 if the file has
// no readable video stream.
int fixedPictures(const QString& path, const std::vector<double>& at, int minKeys, int maxDecode,
                  const std::function<bool(const AVFrame*, int turns)>& onFrame);

} // namespace Video
