// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/SoundSync.h"

#include "engine/audio/Reader.h"

#include <QFileInfo>

#include <algorithm>

namespace SoundSync {

namespace {

constexpr int kChunksPerSecond = 5; // chunks of 0.2 s

// The stream from the picture's first frame (position 0, as the timeline counts) to its end, channels averaged, read
// with the own audio engine. At 48 kHz like the material mostly is
// (resampling to 16 kHz would cut the envelope's top band).
std::optional<std::vector<float>> envelope(const QString& path, int stream, const Progress& progress)
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
    return envelope(path, stream, progress);
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
