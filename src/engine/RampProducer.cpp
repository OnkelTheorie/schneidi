// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/RampProducer.h"
#include "engine/PitchLatency.h"
#include "engine/Profiles.h"

#include "core/Retime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace RampProducer {

namespace {

struct VideoData {
    std::unique_ptr<Mlt::Producer> inner;
    RetimeMap map;
    int fileLength;
};

int videoGetFrame(mlt_producer self, mlt_frame_ptr frame, int index)
{
    auto* d = static_cast<VideoData*>(self->child);
    const mlt_position pos = mlt_producer_position(self);
    const int f = std::clamp(int(std::floor(d->map.fileFrameAt(pos) + 1e-6)), 0, std::max(0, d->fileLength - 1));
    mlt_producer inner = d->inner->get_producer();
    mlt_producer_seek(inner, f);
    mlt_service_get_frame(MLT_PRODUCER_SERVICE(inner), frame, index);
    // Position as with timewarp: that of the ramp producer (keyframe filters at the cut count in it)
    mlt_frame_set_position(*frame, pos);
    mlt_producer_prepare_next(self);
    return 0;
}

void videoClose(mlt_producer self)
{
    delete static_cast<VideoData*>(self->child);
    self->child = nullptr;
    self->close = nullptr;
    mlt_producer_close(self);
    free(self);
}

// Speed as text for timewarp: reads via atof in the current LC_NUMERIC (see docs/dev/mlt.md)
QString warpResource(double speed, const QString& file)
{
    char num[32];
    std::snprintf(num, sizeof num, "%.10g", speed);
    return QString("timewarp:%1:%2").arg(QString::fromLatin1(num), file);
}

} // namespace

std::unique_ptr<Mlt::Producer> video(Mlt::Profile& profile, std::unique_ptr<Mlt::Producer> inner, const Clip& c)
{
    const int fileLength = inner->get_length();
    mlt_producer raw = mlt_producer_new(profile.get_profile());
    if (!raw) return nullptr;
    auto* d = new VideoData{nullptr, RetimeMap(c, fileLength), fileLength};
    d->inner = std::move(inner);
    d->inner->set_speed(0); // we control the position ourselves
    raw->child = d;
    raw->get_frame = videoGetFrame;
    raw->close = reinterpret_cast<mlt_destructor>(videoClose);
    auto p = std::make_unique<Mlt::Producer>(raw); // own reference
    mlt_producer_close(raw);
    const int len = std::max(1, d->map.length());
    p->set("resource", d->inner->get("resource"));
    p->set("mlt_service", "schneidi_ramp");
    p->set("length", len);
    p->set("in", 0);
    p->set("out", len - 1);
    // For sourceAspect (transform preserving aspect ratio), as with the still image
    const int vi = d->inner->get_int("video_index");
    const QByteArray rotate = QString("meta.media.%1.codec.rotate").arg(vi).toUtf8();
    for (const char* name : {"width", "height", "aspect_ratio", "video_index", "meta.media.width", "meta.media.height",
                             rotate.constData()})
        if (const char* v = d->inner->get(name)) p->set(name, v);
    return p;
}

QVector<AudioStep> audioSteps(const Clip& c, int fileLength)
{
    const RetimeMap map(c, fileLength);
    const int total = map.length();
    QVector<AudioStep> steps;
    auto add = [&](int a, int b, bool smooth) {
        if (b <= a) return;
        const double s0 = map.sourceAt(a), s1 = map.sourceAt(b);
        double v = (s1 - s0) / (b - a);
        v = std::round(v * 1000.0) / 1000.0; // equal speeds share one producer
        if (v <= 0) v = 0.001;
        steps << AudioStep{a, b, s0, v, smooth};
    };
    for (const RetimeMap::Piece& p : map.pieces()) {
        const int a = int(std::lround(p.m0));
        const int b = std::isfinite(p.m1) ? std::min(total, int(std::lround(p.m1))) : total;
        if (b <= a) continue;
        if (p.v0 == p.v1) {
            add(a, b, false);
            continue;
        }
        // smooth transition: steps of about 1/8 of the length, at least 2 frames
        const int step = std::max(2, (b - a) / 8);
        for (int x = a; x < b; x += step) add(x, std::min(b, x + step), true);
    }
    return steps;
}

std::unique_ptr<Mlt::Producer> audio(Mlt::Profile& profile, const QString& file, const Clip& c)
{
    Mlt::Producer probe(profile, file.toUtf8().constData());
    if (!probe.is_valid()) return nullptr;
    const int fileLength = probe.get_length();
    // Stream choice and sample rate from the opened file: the step producers below open lazily and have no
    // stream metadata yet
    probe.set("video_index", -1);
    selectAudioStream(probe, c.audioStream);
    const int audioIndex = probe.get_int("audio_index");
    const int sampleRate = PitchLatency::sampleRateOf(probe);
    auto pl = std::make_unique<Mlt::Playlist>(profile);
    std::map<std::pair<double, bool>, std::unique_ptr<Mlt::Producer>> byWarp; // (speed with direction, pitch)
    for (const AudioStep& st : audioSteps(c, fileLength)) {
        const double warp = c.reverse ? -st.speed : st.speed;
        const bool pitch = c.keepPitch && !st.smooth && std::abs(warp) != 1.0; // reverse 100 %: without Rubberband
        auto it = byWarp.find({warp, pitch});
        if (it == byWarp.end()) {
            // One producer per speed step: opening a file costs ffmpeg's stream analysis twice (validation, then
            // the real open; ~0.4 s each for 60/120 fps game captures), and a soft ramp has a dozen steps. The file
            // was opened above already, so skip the validation and give the length ourselves.
            const QString lazy = "avformat-novalidate:" + file;
            const QString resource = warp == 1.0 ? lazy : warpResource(warp, lazy);
            auto p = std::make_unique<Mlt::Producer>(profile, resource.toUtf8().constData());
            if (!p->is_valid()) return nullptr;
            const int length = std::max(1, int(fileLength / std::abs(warp)));
            p->set("length", length);
            p->set("out", length - 1);
            if (warp != 1.0) p->set("warp_pitch", pitch ? 1 : 0);
            p->set("video_index", -1);
            p->set("audio_index", audioIndex);
            if (pitch) PitchLatency::extend(*p, PitchLatency::frames(profile, warp, sampleRate));
            it = byWarp.emplace(std::make_pair(warp, pitch), std::move(p)).first;
        }
        Mlt::Producer& p = *it->second;
        const int lead = pitch ? PitchLatency::frames(profile, warp, sampleRate) : 0;
        const int plen = p.get_length() - lead; // without the extension for the lead-in
        // timewarp frame k shows source position k·v; backwards it counts from the end of the warped file
        int k = int(std::lround(st.s0 / st.speed));
        if (c.reverse) k = int(std::lround((plen - 1) - (fileLength - 1 - st.s0) / st.speed));
        const int len = st.m1 - st.m0;
        k = std::clamp(k, 0, std::max(0, plen - len));
        k += lead; // keep pitch: fetch the audio earlier by the Rubberband delay (see PitchLatency)
        pl->append(p, k, k + len - 1);
    }
    return pl;
}

} // namespace RampProducer
