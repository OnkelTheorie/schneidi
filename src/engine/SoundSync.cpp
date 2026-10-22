#include "engine/SoundSync.h"

#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/audio/Path.h"
#include "engine/audio/Reader.h"

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

// Own engine: the stream from the picture's first frame (position 0, as MLT counts) to its end, channels averaged.
// At 48 kHz like the material mostly is (MLT handed out the file's own rate; resampling to 16 kHz would cut the
// envelope's top band).
std::optional<std::vector<float>> envelopeOwn(const QString& path, int stream, const Progress& progress)
{
    constexpr int kRate = 48000;
    const auto reader = Audio::Reader::open(path, stream, Audio::Reader::Options{kRate, 0});
    if (!reader) return std::nullopt;
    const int64_t end = reader->info().end;
    if (end <= 0) return std::nullopt;
    const int channels = reader->channels() > 0 ? reader->channels() : reader->info().sourceChannels;
    if (channels <= 0) return std::nullopt;
    constexpr int kChunk = kRate / kChunksPerSecond;
    std::vector<float> buf(size_t(kChunk) * size_t(channels)), mono(kChunk);
    std::vector<float*> out(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) out[size_t(c)] = buf.data() + size_t(c) * kChunk;
    AudioSync::Envelope env(kRate);
    for (int64_t pos = 0; pos < end; pos += kChunk) {
        if (progress && (pos / kChunk) % 50 == 0 && !progress(double(pos) / double(end))) return std::nullopt;
        const int n = int(std::min<int64_t>(kChunk, end - pos));
        reader->read(pos, n, out.data());
        std::fill(mono.begin(), mono.begin() + n, 0.f);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i) mono[size_t(i)] += out[size_t(c)][i];
        for (int i = 0; i < n; ++i) mono[size_t(i)] /= float(channels);
        env.add(mono.data(), n);
    }
    if (progress && !progress(1.0)) return std::nullopt;
    return env.take();
}

} // namespace

std::optional<std::vector<float>> fileEnvelope(const QString& path, int stream, const Progress& progress)
{
    if (stream < 0 || !QFileInfo(path).isFile()) return std::nullopt;
    if (Audio::ownPath()) return envelopeOwn(path, stream, progress);
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
