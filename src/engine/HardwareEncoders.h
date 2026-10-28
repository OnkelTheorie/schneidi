// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Hardware video encoders for Deliver (NVIDIA NVENC, VAAPI on Linux, Intel Quick Sync).
// `ffmpeg -encoders` lists them even without a matching graphics card, and MLT's avformat consumer crashes when an
// encoder cannot be opened -> every encoder is tested with a real tiny encode (ffmpeg, same options as the export)
// before it is offered or used. Results are cached for the session.

#include "core/RenderJob.h"

#include <QObject>
#include <QPair>
#include <QSize>
#include <QStringList>
#include <QVector>

namespace HardwareEncoders {

// FFmpeg codec of an encoder for a render format ("h264"/"h265"), e.g. h264_nvenc; empty = not for this format.
// Software gives the software codec (softwareCodec).
QString codecName(VideoEncoder e, const QString& format);
// Software video encoder of a render format that this FFmpeg has: the format's own (libx264, libx265, libsvtav1 …)
// or a free replacement when it is missing (the GPL-free build has no x264/x265): OpenH264 for H.264, kvazaar for
// H.265, libaom for AV1. Empty = none (and for audio-only formats).
QString softwareCodec(const QString& format);
// The format can be rendered with this FFmpeg: its audio encoder exists, and a software video encoder or (H.264/H.265)
// a hardware one. Does not wait for the hardware probe: until it has run, only software counts.
bool formatAvailable(const QString& format);
// Encoder options of a software codec for a quality (crf as for x264: 18/22/28) where the codec has no "crf" of its
// own (OpenH264, kvazaar); empty for the others (they take crf)
QVector<QPair<QString, QString>> softwareOptions(const QString& codec, int crf, QSize size, double fps);
// ffmpeg arguments for the video encoder of proxy files (short GOP, no B-frames, fast to decode): x264, else OpenH264
QStringList proxyVideoArguments();
bool isHardwareCodec(const QString& codec);
VideoEncoder encoderOfCodec(const QString& codec); // Software for anything that is not a hardware codec

// VAAPI render device (Linux: first /dev/dri/renderD*; other systems: none -> no VAAPI)
QString vaapiDevice();

// Encoder options for a hardware codec (passed to MLT's avformat consumer and to the ffmpeg test the same way).
// crf = quality of the software encoder (18/22/28): mapped to the encoder's own constant quality scale.
QVector<QPair<QString, QString>> codecOptions(const QString& codec, int crf);
// Pixel format given to MLT ("vaapi" = upload to the VAAPI device in MLT)
QString pixelFormat(const QString& codec);
// ffmpeg arguments for a test encode of a few frames of this size (black picture, output discarded)
QStringList testArguments(const QString& codec, QSize size, int crf);

// Session cache of test results per codec and size (thread-safe); 0 = unknown, 1 = works, -1 = fails
int cachedResult(const QString& codec, QSize size);
void remember(const QString& codec, QSize size, bool works);
// Test encode (blocking, a fraction of a second; uses the cache)
bool test(const QString& codec, QSize size, int crf = 22);

// Which hardware encoders work for a format, in order of preference (NVENC, Quick Sync, VAAPI).
// Blocks until the probe has run (starts it in this thread if it has not started yet).
QVector<VideoEncoder> available(const QString& format);
bool probed();          // probe finished (available() would not block)
void probeInBackground(); // start the probe on a worker thread (no-op when started already)

// Notifies (in the GUI thread) when the background probe has finished
class Notifier : public QObject {
    Q_OBJECT
signals:
    void probed();
};
Notifier* notifier();

// Concrete encoder for a render: wanted = the job's choice. Auto -> first available hardware encoder, else software;
// a hardware encoder that does not work here -> software with a note. Blocks for the probe when needed (Auto or
// a hardware choice).
struct Choice {
    QString codec;    // FFmpeg video codec to use
    QString fallback; // software codec when codec is a hardware one (export falls back to it), else empty
    QString note;     // shown to the user (e.g. "NVENC not available, rendered with software"), may be empty
};
Choice resolve(VideoEncoder wanted, const QString& format);

// For tests: forget all results (the next available() probes again)
void resetForTests();

} // namespace HardwareEncoders
