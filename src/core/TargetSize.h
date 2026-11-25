// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Render to a file size (`render` max_mb, Deliver › "Max. Dateigröße"): the video bit rate that fills a size limit
// (Discord 10 MB, WhatsApp 16 MB, mail 25 MB) from the length, the sound's bit rate and the container's overhead, aiming
// about 3 % under the limit; the picture scaled down in steps (1080 -> 720 -> 540 -> 360) when that rate would be too
// low for its size. Data only: the Exporter encodes at the rate (ExportSettings::targetBitrateK).

#include "core/RenderJob.h"

#include <QSize>
#include <QString>

namespace TargetSize {

// 1 MB = 1,000,000 bytes (a file below it is below 1 MiB too: the safe reading of the messengers' limits)
inline constexpr double kBytesPerMb = 1000.0 * 1000.0;
// The share of the limit aimed at (encoders land a few percent above or below their average rate)
inline constexpr double kAim = 0.97;
// Sound when the caller names none: 128 kbit/s, down to 96 or 64 when it would take more than a fifth of the size
inline constexpr int kAudioDefaultK = 128;
inline constexpr int kAudioMinK = 64;
inline constexpr double kAudioShare = 0.2;
// Below this the encoders make mush: a video under it says so (`poor`)
inline constexpr int kMinVideoK = 30;
// Above this many bits per pixel and frame more rate buys nothing (a huge limit for a short range: the rate stops here)
inline constexpr double kMaxBitsPerPixel = 1.0;

// A format with a bit rate (video with a quality choice: H.264, H.265, AV1, VP9, s264). Else why not (English, for the
// CLI's BAD_ARGUMENT), empty when it takes one
QString refusal(const RenderFormatInfo& f);

struct Input {
    double maxMb = 0;       // the limit
    double seconds = 0;     // length of the range
    double fps = 30;        // output frame rate
    QSize size;             // output size without scaling
    bool scale = true;      // may scale the picture down (false: the caller fixed height/size)
    bool sound = true;      // the file has sound
    int audioK = 0;         // the sound's bit rate asked for (kbit/s)
    bool audioFixed = false; // the caller named it (or the codec has a fixed one, AC-3): kept as it is
    QString extension;      // container ("mp4", "webm", "mkv" …): overhead per packet
    QString videoCodec;     // FFmpeg encoder of the format ("libx264" …): the bits-per-pixel floor
};

struct Plan {
    bool ok = false;
    QString error;          // !ok: why (English, the CLI's BAD_ARGUMENT)
    bool empty = false;     // !ok because there is nothing to render (empty range or size), not because it is too small
    int videoK = 0;         // video bit rate to aim at (kbit/s)
    int audioK = 0;         // sound bit rate (kbit/s), 0 = no sound
    QSize size;             // output size (scaled down or as given)
    bool scaled = false;    // smaller than Input::size
    double bitsPerPixel = 0; // of the video at that size (per frame, high frame rates weighted down like "web")
    double floor = 0;       // the codec's floor: below it the picture is poor
    bool poor = false;      // below the floor even at the smallest step (or scaling not allowed)
    qint64 limitBytes = 0;  // maxMb in bytes
    qint64 overheadBytes = 0; // container overhead estimated
    double seconds = 0;     // the length planned for
};

Plan plan(const Input& in);

// Bits per pixel and frame below which `codec` looks poor (H.264 0.05; H.265, AV1, VP9 0.035)
double floorBitsPerPixel(const QString& codec);
// Bits per pixel and frame of `videoK` at that size and rate (above 31 fps a frame needs fewer, as for quality "web")
double bitsPerPixel(int videoK, QSize size, double fps);

// The first try wrote `fileBytes`, more than the limit: the video rate for one more try, scaled by how much the video
// part has to shrink to land 5 % under the limit. Always lower than plan.videoK.
int retryVideoK(const Plan& plan, qint64 fileBytes);

} // namespace TargetSize
