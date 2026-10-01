#include "engine/PitchLatency.h"

#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

namespace PitchLatency {

namespace {

constexpr double kClickAt = 0.5; // Klick in Timeline-Sekunden (Rubberband braucht davor ein paar Frames Anlauf)
constexpr double kMaxDelay = 1.5;

// Stereo-WAV (16 Bit) mit einem einzelnen Klick bei `clickAt` Sekunden
bool writeClick(const QString& path, int rate, double seconds, double clickAt)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 frames = quint32(seconds * rate), bytes = frames * 4;
    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);
    s.writeRawData("RIFF", 4);
    s << quint32(36 + bytes);
    s.writeRawData("WAVEfmt ", 8);
    s << quint32(16) << quint16(1) << quint16(2) << quint32(rate) << quint32(rate * 4) << quint16(4) << quint16(16);
    s.writeRawData("data", 4);
    s << bytes;
    const quint32 c0 = quint32(clickAt * rate), c1 = c0 + quint32(rate / 500); // 2 ms
    for (quint32 i = 0; i < frames; ++i) {
        const qint16 v = i >= c0 && i < c1 ? 29000 : 0;
        s << v << v;
    }
    return s.status() == QDataStream::Ok;
}

// Erster Klick-Sample (Timeline) mit timewarp ab Material-Frame 0, -1 = nicht gefunden
long clickSample(Mlt::Profile& profile, const QString& file, double speed, bool pitch)
{
    char num[32];
    std::snprintf(num, sizeof num, "%.10g", speed); // timewarp liest per atof (siehe dev-notes)
    Mlt::Producer p(profile, QString("timewarp:%1:%2").arg(QString::fromLatin1(num), file).toUtf8().constData());
    if (!p.is_valid()) return -1;
    p.set("warp_pitch", pitch ? 1 : 0);
    p.set("video_index", -1);
    const double fps = profile.fps();
    const int frames = int(std::ceil((kClickAt + kMaxDelay) * fps));
    long at = 0;
    for (int i = 0; i < frames; ++i) {
        p.seek(i);
        std::unique_ptr<Mlt::Frame> fr(p.get_frame());
        if (!fr) return -1;
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2, samples = mlt_audio_calculate_frame_samples(float(fps), freq, i);
        const auto* pcm = static_cast<const float*>(fr->get_audio(fmt, freq, channels, samples));
        if (pcm && fmt == mlt_audio_float)
            for (int k = 0; k < samples; ++k)
                if (std::abs(pcm[k]) > 0.2f) return at + k; // planar: erster Kanal
        at += samples;
    }
    return -1;
}

int measure(Mlt::Profile& profile, double speed, int rate)
{
    QTemporaryDir dir;
    if (!dir.isValid()) return 0;
    const QString file = dir.filePath("click.wav");
    if (!writeClick(file, rate, (kClickAt + kMaxDelay) * speed + 1.0, kClickAt * speed)) return 0;
    const long plain = clickSample(profile, file, speed, false);
    const long pitched = clickSample(profile, file, speed, true);
    if (plain < 0 || pitched < plain) return 0;
    return int(std::lround(double(pitched - plain) / 48000.0 * profile.fps()));
}

} // namespace

int frames(Mlt::Profile& profile, double speed, int sampleRate)
{
    speed = std::abs(speed);
    if (speed == 1.0 || speed < 0.1) return 0; // timewarp korrigiert unter 10 % nicht (MLT)
    static std::mutex mutex;
    static std::map<std::tuple<double, int, int, int>, int> cache;
    const std::lock_guard<std::mutex> lock(mutex);
    const auto key = std::make_tuple(speed, profile.frame_rate_num(), profile.frame_rate_den(), sampleRate);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, measure(profile, speed, sampleRate)).first;
    return it->second;
}

void extend(Mlt::Producer& p, int lead)
{
    if (lead > 0) p.set("length", p.get_length() + lead);
}

int sampleRateOf(Mlt::Producer& p)
{
    const int index = p.get_int("audio_index");
    const int rate = index >= 0 ? p.get_int(QString("meta.media.%1.codec.sample_rate").arg(index).toUtf8().constData()) : 0;
    return rate > 0 ? rate : 48000;
}

} // namespace PitchLatency
