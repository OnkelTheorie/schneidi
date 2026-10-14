#include "engine/SoundSync.h"

#include "core/ProjectFormat.h"
#include "engine/Profiles.h"

#include <Mlt.h>
#include <QFileInfo>

#include <algorithm>
#include <memory>

namespace SoundSync {

namespace {

constexpr int kDecodeRate = 16000; // enough for the onsets, a third of the work of 48 kHz
constexpr int kChunksPerSecond = 5; // MLT frames of 0.2 s: fewer calls than video frames, picture is never decoded

std::unique_ptr<Mlt::Profile> decodeProfile()
{
    ProjectFormat f;
    f.width = 64;
    f.height = 36;
    f.rate = {kChunksPerSecond, 1};
    return makeProfile(f);
}

} // namespace

std::optional<std::vector<float>> fileEnvelope(const QString& path, int stream, const Progress& progress)
{
    if (stream < 0 || !QFileInfo(path).isFile()) return std::nullopt;
    const auto profile = decodeProfile();
    Mlt::Producer p(*profile, path.toUtf8().constData());
    if (!p.is_valid() || p.get_int("audio_index") < 0) return std::nullopt;
    if (stream >= audioStreamsOf(p).size() && stream > 0) return std::nullopt;
    selectAudioStream(p, stream);
    if (p.get_int("audio_index") < 0) return std::nullopt;
    p.set("video_index", -1); // sound only: never decode the picture

    const int length = p.get_length();
    if (length <= 0) return std::nullopt;
    std::optional<AudioSync::Envelope> env;
    std::vector<float> mono;
    bool any = false;
    p.seek(0);
    for (int pos = 0; pos < length; ++pos) {
        if (progress && pos % 50 == 0 && !progress(double(pos) / length)) return std::nullopt;
        std::unique_ptr<Mlt::Frame> f(p.get_frame());
        if (!f) break;
        mlt_audio_format fmt = mlt_audio_float; // planar
        int freq = kDecodeRate, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(kChunksPerSecond), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (!pcm || samples <= 0 || channels <= 0 || fmt != mlt_audio_float || freq <= 0) continue;
        if (!env) env.emplace(freq); // the rate MLT really delivers (no resampler: the file's own)
        mono.assign(size_t(samples), 0.f);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < samples; ++i) mono[size_t(i)] += pcm[size_t(c) * size_t(samples) + size_t(i)];
        for (float& s : mono) s /= float(channels);
        env->add(mono.data(), samples);
        any = true;
    }
    if (progress && !progress(1.0)) return std::nullopt;
    if (!any || !env) return std::nullopt;
    return env->take();
}

QVector<AudioSync::Match> align(const QVector<Input>& files, const AudioSync::Options& options,
                                const Progress& progress)
{
    // Decoding is nearly all of the time: 95 % for the files (equal shares), the rest for matching
    QVector<std::vector<float>> envelopes;
    const double share = files.isEmpty() ? 0.0 : 0.95 / files.size();
    for (int i = 0; i < files.size(); ++i) {
        bool canceled = false;
        const auto e = fileEnvelope(files[i].path, files[i].stream, [&](double f) {
            if (!progress || progress(share * (i + std::clamp(f, 0.0, 1.0)))) return true;
            canceled = true;
            return false;
        });
        if (canceled) return {};
        envelopes << e.value_or(std::vector<float>{});
    }
    if (progress && !progress(0.95)) return {};
    QVector<AudioSync::Match> out = AudioSync::alignAll(envelopes, options);
    // A file without sound is never found (also not as the reference)
    for (int i = 0; i < out.size(); ++i)
        if (envelopes[i].empty()) out[i] = AudioSync::Match{};
    if (progress) progress(1.0);
    return out;
}

} // namespace SoundSync
