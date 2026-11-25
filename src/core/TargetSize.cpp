// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TargetSize.h"

#include <algorithm>
#include <cmath>

namespace TargetSize {

namespace {

// Container bytes besides the streams: headers and index (a fixed part) and per packet (MP4's sample tables, Matroska's
// block headers: ~10-20 bytes each), the sound in packets of ~20 ms
constexpr qint64 kFixedOverhead = 16 * 1024;
constexpr int kBytesPerPacket = 20;
constexpr double kAudioPacketsPerSecond = 50;

// The picture's shorter edge in steps, largest first
constexpr int kSteps[] = {1080, 720, 540, 360};

QSize scaledTo(QSize size, int shortSide)
{
    const int s = std::min(size.width(), size.height());
    if (s <= 0 || shortSide >= s) return size;
    const double f = double(shortSide) / s;
    const auto even = [](double v) { return std::max(16, int(std::lround(v / 2)) * 2); };
    return {even(size.width() * f), even(size.height() * f)};
}

qint64 overheadBytes(double seconds, double fps, bool sound)
{
    const double packets = seconds * fps + (sound ? seconds * kAudioPacketsPerSecond : 0);
    return kFixedOverhead + qint64(std::ceil(packets * kBytesPerPacket));
}

double audioBytes(int kbit, double seconds) { return kbit * 1000.0 / 8 * seconds; }

} // namespace

QString refusal(const RenderFormatInfo& f)
{
    switch (f.kind) {
    case RenderKind::Audio: return QStringLiteral("%1 is audio only (set audio_bitrate for its size)").arg(f.id);
    case RenderKind::Gif: return QStringLiteral("GIF has no bit rate (make it smaller with height or fps)");
    case RenderKind::ImageSequence: return QStringLiteral("%1 writes single pictures, not one file").arg(f.id);
    case RenderKind::Video: break;
    }
    if (!f.hasQuality())
        return QStringLiteral("%1 has a fixed quality and no bit rate (use h264, h265, av1, vp9 or s264)").arg(f.id);
    return {};
}

double floorBitsPerPixel(const QString& codec)
{
    // H.264 at 0.05 (1080p30 ~3 Mbit/s, 720p30 ~1.4): still clean for calm pictures, blocky in fast games; the newer
    // codecs need about 30 % less for the same look
    return codec.contains(QLatin1String("264")) ? 0.05 : 0.035; // libx264, h264_nvenc, libopenh264, s264 …
}

double bitsPerPixel(int videoK, QSize size, double fps)
{
    const double perFrame = std::max(1.0, double(size.width()) * size.height() * std::max(1.0, fps * (fps > 31 ? 0.76 : 1.0)));
    return videoK * 1000.0 / perFrame;
}

Plan plan(const Input& in)
{
    Plan p;
    if (!(in.maxMb > 0) || !(in.seconds > 0) || in.size.isEmpty()) {
        p.error = QStringLiteral("nothing to render for max_mb (empty range or size)");
        return p;
    }
    p.limitBytes = qint64(std::floor(in.maxMb * kBytesPerMb));
    p.seconds = in.seconds;
    const double aim = p.limitBytes * kAim;
    p.overheadBytes = overheadBytes(in.seconds, in.fps, in.sound);
    // Sound: as named, else 128 kbit/s and less when it would take more than a fifth of the size
    if (in.sound) {
        p.audioK = in.audioK > 0 ? in.audioK : kAudioDefaultK;
        if (!in.audioFixed) {
            p.audioK = std::min(p.audioK, kAudioDefaultK);
            while (p.audioK > kAudioMinK && audioBytes(p.audioK, in.seconds) > aim * kAudioShare)
                p.audioK = p.audioK > 96 ? 96 : kAudioMinK;
        }
    }
    const double videoBytes = aim - p.overheadBytes - audioBytes(p.audioK, in.seconds);
    p.videoK = int(std::floor(std::max(0.0, videoBytes) * 8 / in.seconds / 1000));
    p.floor = floorBitsPerPixel(in.videoCodec);
    if (p.videoK < kMinVideoK) {
        // What a watchable small picture would need (360p at the floor, sound at its least)
        const QSize small = scaledTo(in.size, 360);
        const double fpsWeight = in.fps * (in.fps > 31 ? 0.76 : 1.0);
        const double need = (p.floor * small.width() * small.height() * fpsWeight / 8 * in.seconds
                             + (in.sound ? audioBytes(in.audioFixed ? p.audioK : kAudioMinK, in.seconds) : 0)
                             + p.overheadBytes)
                            / kAim / kBytesPerMb;
        p.error = QStringLiteral("max_mb %1 is too small for %2 s (%3 kbit/s left for the picture%4): a watchable %5p "
                                 "needs about %6 MB - shorten the range or raise max_mb")
                      .arg(in.maxMb)
                      .arg(in.seconds, 0, 'f', 1)
                      .arg(p.videoK)
                      .arg(in.sound ? QStringLiteral(" beside %1 kbit/s sound").arg(p.audioK) : QString())
                      .arg(std::min(360, std::min(in.size.width(), in.size.height())))
                      .arg(std::ceil(need * 10) / 10);
        return p;
    }
    // Size: the first step whose bits per pixel reach the floor (the given size first)
    QVector<QSize> sizes{in.size};
    const int shortSide = std::min(in.size.width(), in.size.height());
    if (in.scale)
        for (int step : kSteps)
            if (step < shortSide) sizes << scaledTo(in.size, step);
    p.size = sizes.last();
    for (const QSize& s : sizes)
        if (bitsPerPixel(p.videoK, s, in.fps) >= p.floor) {
            p.size = s;
            break;
        }
    p.scaled = p.size != in.size;
    p.bitsPerPixel = bitsPerPixel(p.videoK, p.size, in.fps);
    p.poor = p.bitsPerPixel < p.floor;
    p.ok = true;
    return p;
}

int retryVideoK(const Plan& plan, qint64 fileBytes)
{
    // What is not video, as estimated; the video part as it came out, and as it may be
    const double other = audioBytes(plan.audioK, plan.seconds) + plan.overheadBytes;
    const double video = std::max(1.0, fileBytes - other);
    const double allowed = std::max(1.0, plan.limitBytes * 0.95 - other);
    const int k = int(std::floor(plan.videoK * std::min(allowed / video, 0.97)));
    return std::max(1, std::min(k, plan.videoK - 1));
}

} // namespace TargetSize
