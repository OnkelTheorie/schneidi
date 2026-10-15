#include "engine/TimelineBuilder.h"
#include "core/Dynamics.h"
#include "core/Equalizer.h"
#include "core/Limiter.h"
#include "core/Loudness.h"
#include "core/Multicam.h"
#include "core/TruePeak.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Denoise.h"
#include "engine/Lumas.h"
#include "engine/Profiles.h"
#include "engine/PitchLatency.h"
#include "engine/RampProducer.h"
#include "engine/Stabilizer.h"

#include <Mlt.h>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

#include "engine/TimelineBuilderDetail.h"

using namespace TimelineBuilderDetail;

namespace {

// Producer property: frames by which cutOf fetches the audio earlier (delay of "keep pitch", PitchLatency)
constexpr const char* kPitchLead = "schneidi.pitch_lead";

} // namespace

TimelineBuilder::TimelineBuilder(Mlt::Profile& profile)
    : m_profile(profile), m_factory(std::make_unique<ProducerFactory>(profile))
{
}
TimelineBuilder::~TimelineBuilder() = default;

void TimelineBuilder::setDecoderThreads(int threads)
{
    m_factory->setThreads(threads);
}

Mlt::Producer* TimelineBuilder::producerFor(const QString& path, TrackKind kind, int trackIndex, bool second,
                                            const Clip* retime)
{
    // Proxy (preview only): key with the file actually read, so that switching reopens
    const QString file = m_resolver ? m_resolver(path, kind) : path;
    // Speed: MLT timewarp (negative = reverse); still image: repeated single frame (see below)
    const bool freeze = retime && retime->freeze;
    const bool ramp = retime && retime->hasRamp(); // Speed ramp: own producer (engine/RampProducer)
    const double warp = retime && !freeze && !ramp ? (retime->reverse ? -retime->speed : retime->speed) : 1.0;
    const bool pitch = retime && retime->keepPitch;
    QString resource = file;
    QString tag;
    if (ramp) {
        tag = QString("|r%1%2%3|%4").arg(retime->speed).arg(retime->reverse ? "r" : "").arg(pitch ? "p" : "");
        for (const SpeedPoint& sp : retime->ramp) tag += QString("|%1:%2:%3").arg(sp.source).arg(sp.speed).arg(sp.smooth);
    }
    if (warp != 1.0) {
        // timewarp reads the number via atof -> write it in the currently valid C locale (comma/dot, see docs/dev/mlt.md)
        char num[32];
        std::snprintf(num, sizeof num, "%.10g", warp);
        resource = QString("timewarp:%1:%2").arg(QString::fromLatin1(num), file);
        tag = QString("|w%1%2").arg(warp).arg(pitch ? "p" : "");
    }
    if (freeze) tag = QString("|f%1|%2%3").arg(retime->stillFrame()).arg(retime->speed).arg(retime->reverse ? "r" : "");
    // Audio stream (OBS: several per file) is part of the key: every stream needs its own producer
    const int stream = kind == TrackKind::Audio && retime ? retime->audioStream : 0;
    if (stream != 0) tag += QString("|s%1").arg(stream);
    const QString key = m_keyPrefix + QString("%1%2|%3|%4%5").arg(kind == TrackKind::Video ? "v" : "a").arg(second ? "x" : "")
                            .arg(trackIndex).arg(file, tag);
    m_used.insert(key);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();

    if (ramp) {
        std::unique_ptr<Mlt::Producer> r;
        if (kind == TrackKind::Video) {
            auto inner = m_factory->open(file);
            if (!inner->is_valid()) return nullptr;
            inner->set("audio_index", -1);
            r = RampProducer::video(m_profile, std::move(inner), *retime);
        } else {
            r = RampProducer::audio(m_profile, file, *retime);
        }
        if (!r || !r->is_valid()) return nullptr;
        Mlt::Producer* raw = r.get();
        m_cache.emplace(key, std::move(r));
        return raw;
    }
    // Video via the factory (BT.601 sources, see Profiles.h); audio does not need that
    auto p = kind == TrackKind::Video ? m_factory->open(resource)
                                      : std::make_unique<Mlt::Producer>(m_profile, resource.toUtf8().constData());
    if (!p->is_valid()) return nullptr;
    // Keep pitch only on a real speed change: reverse at 100 % needs no Rubberband, which would only bring
    // its delay (audio ~3 frames behind the video)
    const bool warpPitch = warp != 1.0 && kind == TrackKind::Audio && pitch && std::abs(warp) != 1.0;
    if (warp != 1.0 && kind == TrackKind::Audio) p->set("warp_pitch", warpPitch ? 1 : 0);
    // Do not decode an unneeded stream at all
    if (kind == TrackKind::Video) {
        p->set("audio_index", -1);
    } else {
        p->set("video_index", -1);
        selectAudioStream(*p, stream);
    }
    // Delay of the pitch correction: cuts fetch the audio that many frames earlier (cutOf)
    if (warpPitch) {
        const int lead = PitchLatency::frames(m_profile, warp, PitchLatency::sampleRateOf(*p));
        p->set(kPitchLead, lead);
        PitchLatency::extend(*p, lead);
    }
    // Still images can be stretched arbitrarily long (otherwise MLT limits to the default length)
    if (const QByteArray svc = p->get("mlt_service"); svc == "qimage" || svc == "pixbuf") {
        const int len = 24 * 3600 * qRound(m_profile.fps());
        p->set("length", len);
        p->set("out", len - 1);
    }
    if (freeze) {
        // Still image: frame `in` (converted to file frames) as a 1-frame excerpt, repeated arbitrarily often
        // in a playlist. (MLT hold scales wrongly in the tractor, the kdenlive filter freeze crashed.)
        const int fileLen = p->get_length();
        const double at = retime->stillFrame() * retime->speed;
        const int frame = std::clamp(int(retime->reverse ? fileLen - 1 - at : at), 0, std::max(0, fileLen - 1));
        auto hold = std::make_unique<Mlt::Playlist>(m_profile);
        std::unique_ptr<Mlt::Producer> one(p->cut(frame, frame)); // cut holds a reference to p
        hold->append(*one);
        hold->repeat(0, 24 * 3600 * qRound(m_profile.fps()));
        // For sourceAspect (transform preserving aspect ratio)
        const int vi = p->get_int("video_index");
        const QByteArray rotate = QString("meta.media.%1.codec.rotate").arg(vi).toUtf8();
        for (const char* name : {"width", "height", "aspect_ratio", "video_index", "meta.media.width",
                                 "meta.media.height", rotate.constData()})
            if (const char* v = p->get(name)) hold->set(name, v);
        p = std::move(hold);
    }
    Mlt::Producer* raw = p.get();
    m_cache.emplace(key, std::move(p));
    return raw;
}

void TimelineBuilder::noteUse(Mlt::Producer* p, TrackKind kind, int from, int to, int in)
{
    if (m_inNested || to <= from) return; // inside, the frames of the sequence count, not of the timeline
    LastUse& last = m_lastUse[p];
    if (in != last.sourceEnd + 1)
        m_preroll.push_back({from, last.timelineEnd, in, kind == TrackKind::Audio,
                             std::make_shared<Mlt::Producer>(p->get_producer())});
    last = {in + (to - from) - 1, to};
}

Mlt::Producer* TimelineBuilder::clipAudioSource(const Clip& c)
{
    if (c.isTitle() || c.freeze || c.mediaPath.isEmpty()) return nullptr;
    m_used.clear(); // only this one producer stays in the cache
    Mlt::Producer* p = producerFor(c.mediaPath, TrackKind::Audio, 0, false, &c);
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = m_used.count(it->first) ? std::next(it) : m_cache.erase(it);
    return p;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::buildClipOutput(const Clip& clip)
{
    Clip c = clip;
    c.start = 0;
    c.enabled = true;
    c.fadeIn = c.fadeOut = 0;
    c.transIn = c.transOut = 0;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    tl.masterLimiter = false;
    m_transparent = true;
    auto tractor = build(tl);
    m_transparent = false;
    return tractor;
}

bool TimelineBuilder::applyMixer(const Timeline& tl, MixerHooks& hooks)
{
    if (int(hooks.tracks.size()) != tl.audio.size() || !hooks.master.volume) return false;
    for (const MixerHooks::Strip& h : hooks.tracks)
        if (!h.volume || !h.pan || !h.playlist || !h.dsp) return false;
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    for (int i = 0; i < tl.audio.size(); ++i) {
        MixerHooks::Strip& h = hooks.tracks[i];
        const bool audible = !tl.audio[i].muted && (!anySolo || tl.audio[i].solo);
        h.playlist->set("hide", audible ? 1 : 3); // 1 = no video, 2 = no audio (as when building)
        h.audible = audible;
        h.volume->set("level", mltLevel(tl.audio[i].volumeDb));
        h.pan->set("start", mltPan(tl.audio[i].pan));
        setTrackDsp(*h.dsp, tl.audio[i].eq, tl.audio[i].dynamics);
    }
    hooks.master.volume->set("level", mltLevel(tl.masterVolumeDb));
    if (hooks.master.limiter) setLimiter(*hooks.master.limiter, tl.masterLimiter, tl.masterLimiterDb);
    return true;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::build(const Timeline& tl, MixerHooks* hooks)
{
    if (hooks) *hooks = {};
    m_used.clear();
    m_preroll.clear();
    m_lastUse.clear();
    m_nestedCache.clear();
    m_nested = tl.nested;
    struct BypassScope { // Color correction filters get the switch of this builder
        explicit BypassScope(std::shared_ptr<std::atomic<bool>> f) { t_gradeBypass = std::move(f); }
        ~BypassScope() { t_gradeBypass.reset(); }
    } bypassScope(m_gradeBypass);
    auto tractor = buildTimeline(tl, hooks);
    m_nested.reset();
    m_lastUse.clear();
    std::stable_sort(m_preroll.begin(), m_preroll.end(),
                     [](const PrerollPoint& a, const PrerollPoint& b) { return a.frame < b.frame; });
    m_nestedCache.clear(); // Cuts hold their nested tractors themselves (MLT reference counting)
    // Close producers no longer used (e.g. original after switching to the proxy, or a deleted clip);
    // cuts in the old tractor keep their source alive themselves via MLT reference counting
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = m_used.count(it->first) ? std::next(it) : m_cache.erase(it);
    return tractor;
}

Mlt::Producer* TimelineBuilder::nestedProducer(int sequenceId, TrackKind kind, int trackIndex, bool second, int angle)
{
    if (!m_nested || m_nestStack.size() >= 16 || m_nestStack.count(sequenceId)) return nullptr; // loop/too deep
    const auto it = m_nested->constFind(sequenceId);
    if (it == m_nested->cend()) return nullptr;
    // As with files: one own tractor per track and side of a transition (own decoders)
    QString key = QString("%1|%2|%3|%4|%5").arg(sequenceId).arg(kind == TrackKind::Video ? "v" : "a")
                      .arg(trackIndex).arg(second ? "x" : "").arg(m_nestStack.size());
    if (angle >= 0) key += QString("|m%1").arg(angle);
    if (auto c = m_nestedCache.find(key); c != m_nestedCache.end()) return c->second.get();
    Timeline content;
    if (angle >= 0) { // Multicam segment: only the angle's own tracks
        if (angle >= it->angles.size()) return nullptr;
        content = Multicam::angleTimeline(*it, it->angles[angle], kind);
        if (content.tracks(kind).isEmpty()) return nullptr; // e.g. picture of a sound-only angle
    }
    m_nestStack.insert(sequenceId);
    std::unique_ptr<Mlt::Producer> p = buildInner(angle >= 0 ? content : *it, m_keyPrefix + "n" + key + "/");
    m_nestStack.erase(sequenceId);
    if (!p || !p->is_valid()) return nullptr;
    Mlt::Producer* raw = p.get();
    m_nestedCache.emplace(key, std::move(p));
    return raw;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::buildInner(const Timeline& tl, const QString& keyPrefix)
{
    const bool transparent = std::exchange(m_transparent, true); // empty spots show the track below
    const bool subtitles = std::exchange(m_subtitles, false);    // subtitles belong to the outer timeline
    const bool nested = std::exchange(m_inNested, true);
    // producers inside separate from those of the outer timeline (own decoders, see producerFor)
    const QString prefix = std::exchange(m_keyPrefix, keyPrefix);
    std::unique_ptr<Mlt::Tractor> p = buildTimeline(tl, nullptr);
    m_keyPrefix = prefix;
    m_inNested = nested;
    m_subtitles = subtitles;
    m_transparent = transparent;
    return p;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::buildTimeline(const Timeline& tl, MixerHooks* hooks)
{
    // Solo like DaVinci: as soon as one track has solo, all other audio tracks are muted
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    auto tractor = std::make_unique<Mlt::Tractor>(m_profile);
    // Nested: background far beyond the end, so that a compound clip after shortening its content
    // (clip longer than the sequence) yields empty frames instead of repeats
    const int content = std::max(1, TimelineOps::endFrame(tl));
    const int end = m_inNested ? content + 3600 * qRound(m_profile.fps()) : content;

    // Track 0: black background over the whole length
    Mlt::Playlist background(m_profile);
    Mlt::Producer black(m_profile, m_transparent ? "color:#00000000" : "color:black");
    black.set("length", end);
    background.append(black, 0, end - 1);
    tractor->set_track(background, 0);

    int mltIndex = 1;
    auto fill = [&](const Track& track, TrackKind kind, int trackIndex) {
        Mlt::Playlist pl(m_profile);
        auto isStill = [](Mlt::Producer* p) {
            const QByteArray svc = p->get("mlt_service");
            return svc == "qimage" || svc == "pixbuf";
        };
        const TimelineOps::SourceLength srcLen = [&](const Clip& c) {
            if (c.isNested()) {
                if (!m_nested) return 0;
                const auto it = m_nested->constFind(c.sequenceId);
                return it == m_nested->cend() ? 0 : TimelineOps::endFrame(*it);
            }
            Mlt::Producer* p = c.mediaPath.isEmpty() ? nullptr : producerFor(c.mediaPath, kind, trackIndex); // empty = title
            return p && !isStill(p) ? c.retimedLength(p->get_length()) : 0;
        };
        const QVector<TimelineOps::TransitionSpan> spans = TimelineOps::transitions(track, srcLen);
        QHash<int, const Clip*> byId;
        for (const Clip& c : track.clips) byId.insert(c.id, &c);
        // Producer per clip (first/second of the same file): a clip reads transition and body from the same decoder
        // (no jump at the transition end). If a clip of the same file follows with a crossfade or a different source position,
        // it takes the other one -> that one is free beforehand and can pre-seek to the spot (PrerollPoint).
        // Gaplessly continuing material (split clip) stays on the same one and keeps reading in one go.
        QHash<int, bool> useSecond;
        for (int i = 1; i < track.clips.size(); ++i) {
            const Clip& prev = track.clips[i - 1];
            const Clip& c = track.clips[i];
            if (c.mediaPath.isEmpty() || c.mediaPath != prev.mediaPath) continue;
            const bool dissolve = std::any_of(spans.begin(), spans.end(),
                                              [&](const auto& s) { return s.leftId == prev.id && s.rightId == c.id; });
            const bool seamless = !dissolve && c.start == prev.end() && c.in == prev.out + 1;
            useSecond.insert(c.id, seamless ? useSecond.value(prev.id) : !useSecond.value(prev.id));
        }

        // Per clip, on all its cuts (their state runs on over cut boundaries): noise reduction, then the
        // sub-frame delay (fine position)
        QHash<int, std::shared_ptr<Mlt::Filter>> delays, denoisers;
        auto delayed = [&](const Clip& c, Mlt::Producer* cut) {
            if (!cut || kind != TrackKind::Audio) return cut;
            if (EffectRegistry::has(c, Denoise::EffectId)) {
                auto& f = denoisers[c.id];
                if (!f) f = Denoise::make(m_profile, c);
                if (f) cut->attach(*f);
            }
            if (c.subframe <= 0) return cut;
            auto& f = delays[c.id];
            if (!f) f = makeSubframeDelay(m_profile, c.subframe);
            if (f) cut->attach(*f);
            return cut;
        };
        // Excerpt [from, to) of the timeline from clip c (may extend beyond In/Out into the handles)
        auto rawCutOf = [&](const Clip& c, int from, int to, bool second) -> Mlt::Producer* {
            if (c.isTitle()) {
                if (!c.enabled || kind != TrackKind::Video) return nullptr;
                Mlt::Producer* cut = titleCut(m_profile, c, from - c.start, to - from);
                decorate(m_profile, *cut, c, kind, from - c.start, to - from);
                applyClipFades(m_profile, *cut, c, kind, from, to);
                return cut;
            }
            if (c.isNested()) { // Content of the sequence as its own tractor (frames count from the sequence start)
                const int angle = c.isMulticam() ? std::max(0, c.angle) : -1; // multicam: only the active angle
                Mlt::Producer* src = c.enabled ? nestedProducer(c.sequenceId, kind, trackIndex, second, angle) : nullptr;
                const int in = c.in + (from - c.start);
                if (!src || in < 0) return nullptr;
                Mlt::Producer* cut = src->cut(in, in + (to - from) - 1);
                decorate(m_profile, *cut, c, kind, from - c.start, to - from);
                applyClipFades(m_profile, *cut, c, kind, from, to);
                return cut;
            }
            if (c.freeze && kind == TrackKind::Audio) return nullptr; // Still image is muted (like DaVinci)
            // Render cache (preview only): finished clip output instead of original + effects; frame 0 = clip start.
            // Only inside the clip (transitions with handles before/after keep coming from the original).
            // With Before/After the cache contains the color correction -> such clips then from the original.
            const bool gradeBypassed = m_gradeBypass && m_gradeBypass->load()
                && std::any_of(c.effects.begin(), c.effects.end(),
                               [](const EffectInstance& e) { return e.enabled && e.effectId == QLatin1String("grade"); });
            if (kind == TrackKind::Video && m_clipCache && c.enabled && !gradeBypassed && from >= c.start && to <= c.end()) {
                if (const QString file = m_clipCache(c); !file.isEmpty()) {
                    if (Mlt::Producer* cache = producerFor(file, kind, trackIndex, second)) {
                        Mlt::Producer* cut = cache->cut(from - c.start, to - c.start - 1);
                        noteUse(cache, kind, from, to, from - c.start);
                        applyClipFades(m_profile, *cut, c, kind, from, to); // Fades are not in the cache
                        return cut;
                    }
                }
            }
            Mlt::Producer* src = c.enabled ? producerFor(c.mediaPath, kind, trackIndex, second, &c) : nullptr;
            if (!src) return nullptr;
            int in = c.in + (from - c.start);
            if (in < 0 && isStill(src)) in = 0; // Still image: every frame the same
            if (in < 0) return nullptr;
            // Keep pitch: fetch the audio earlier by the Rubberband delay
            in += src->get_int(kPitchLead);
            Mlt::Producer* cut = src->cut(in, in + (to - from) - 1);
            noteUse(src, kind, from, to, in);
            decorate(m_profile, *cut, c, kind, from - c.start, to - from);
            applyClipFades(m_profile, *cut, c, kind, from, to);
            return cut;
        };
        auto cutOf = [&](const Clip& c, int from, int to, bool second) {
            return delayed(c, rawCutOf(c, from, to, second));
        };

        int cursor = 0;
        auto blankTo = [&](int frame) {
            if (frame > cursor) pl.blank(frame - cursor - 1);
            cursor = std::max(cursor, frame);
        };
        auto appendSpan = [&](const TimelineOps::TransitionSpan& s) {
            blankTo(s.start);
            const Clip* a = byId.value(s.leftId);
            const Clip* b = byId.value(s.rightId);
            std::unique_ptr<Mlt::Producer> ca(a ? cutOf(*a, s.start, s.end, useSecond.value(a->id)) : nullptr);
            std::unique_ptr<Mlt::Producer> cb(b ? cutOf(*b, s.start, s.end, useSecond.value(b->id)) : nullptr);
            const int len = s.length();
            // Gradient wipe from/to nothing: luma has no transparency -> fade out/in like Cross Dissolve
            const bool styled = s.style.type != TransitionType::CrossDissolve && (!s.style.isLuma() || (ca && cb));
            if (kind == TrackKind::Video && styled && (ca || cb)) {
                pl.append(*styledTransition(m_profile, ca.get(), cb.get(), len, s.style));
            } else if (ca && cb) {
                // Cross Dissolve: both sides in a small tractor, crossfade from a to b
                Mlt::Tractor mix(m_profile);
                mix.set_track(*ca, 0);
                mix.set_track(*cb, 1);
                Mlt::Transition t(m_profile, kind == TrackKind::Video ? "luma" : "mix");
                if (kind == TrackKind::Audio) {
                    // Crossfade: both sides with their own curve, then simply add
                    applyAudioTransition(m_profile, *ca, s, a->id);
                    applyAudioTransition(m_profile, *cb, s, b->id);
                    t.set("start", 1.0);
                    t.set("sum", 1);
                }
                t.set_in_and_out(0, len - 1);
                mix.plant_transition(t, 0, 1);
                pl.append(mix);
            } else if (ca || cb) {
                // Only one side (cut to nothing, or the other clip disabled/offline): fade out/in
                Mlt::Producer& one = ca ? *ca : *cb;
                if (kind == TrackKind::Video) applyFade(m_profile, one, len, !ca);
                else applyAudioTransition(m_profile, one, s, ca ? a->id : b->id);
                pl.append(one);
            } else {
                pl.blank(len - 1);
            }
            cursor = s.end;
        };

        for (const Clip& c : track.clips) {
            int bodyStart = c.start, bodyEnd = c.end();
            for (const auto& s : spans) {
                if (s.rightId == c.id) bodyStart = std::max(bodyStart, s.end);
                if (s.leftId == c.id) bodyEnd = std::min(bodyEnd, s.start);
            }
            // Fade in (transition that starts in this clip and has no left clip)
            for (const auto& s : spans)
                if (s.rightId == c.id && !s.leftId) appendSpan(s);
            if (bodyEnd > bodyStart) {
                blankTo(bodyStart);
                if (std::unique_ptr<Mlt::Producer> body(cutOf(c, bodyStart, bodyEnd, useSecond.value(c.id))); body) pl.append(*body);
                else pl.blank(bodyEnd - bodyStart - 1); // disabled or file missing -> gap
                cursor = bodyEnd;
            }
            // Fade out resp. crossfade to the next clip
            for (const auto& s : spans)
                if (s.leftId == c.id) appendSpan(s);
        }
        // hide: 1 = no video, 2 = no audio
        int hide = (kind == TrackKind::Video) ? 2 : 1;
        if (kind == TrackKind::Video && track.hidden) hide |= 1;
        if (kind == TrackKind::Audio && (track.muted || (anySolo && !track.solo))) hide |= 2;
        pl.set("hide", hide);
        if (kind == TrackKind::Audio) {
            MixerHooks::Strip* h = nullptr;
            if (hooks) {
                h = &hooks->tracks.emplace_back();
                h->audible = !(hide & 2);
                h->playlist = std::make_shared<Mlt::Playlist>(pl);
            }
            attachStrip(m_profile, pl, track.volumeDb, track.pan, h, {}, &track);
        }
        tractor->set_track(pl, mltIndex);

        if (kind == TrackKind::Video) {
            Mlt::Transition t(m_profile, "qtblend");
            t.set("always_active", 1);
            tractor->plant_transition(t, 0, mltIndex);
        } else {
            Mlt::Transition t(m_profile, "mix");
            t.set("always_active", 1);
            t.set("sum", 1);
            tractor->plant_transition(t, 0, mltIndex);
        }
        ++mltIndex;
    };

    for (int i = 0; i < tl.video.size(); ++i) fill(tl.video[i], TrackKind::Video, i);
    // Subtitles above all video tracks (visible track; export only when burning in)
    if (m_subtitles)
        for (const SubtitleTrack& st : tl.subtitles) {
            if (!st.enabled || st.cues.isEmpty()) continue;
            Mlt::Playlist pl(m_profile);
            int cursor = 0;
            for (const SubtitleCue& c : st.cues) {
                if (c.end <= cursor || c.text.trimmed().isEmpty()) continue;
                const int from = std::max(cursor, c.start);
                if (from > cursor) pl.blank(from - cursor - 1);
                std::unique_ptr<Mlt::Producer> cut(subtitleCut(m_profile, st.style, c.text, c.end - from));
                pl.append(*cut);
                cursor = c.end;
            }
            pl.set("hide", 2); // no audio
            tractor->set_track(pl, mltIndex);
            Mlt::Transition t(m_profile, "qtblend");
            t.set("always_active", 1);
            tractor->plant_transition(t, 0, mltIndex);
            ++mltIndex;
        }
    for (int i = 0; i < tl.audio.size(); ++i) fill(tl.audio[i], TrackKind::Audio, i);

    // Master fader on the tractor (thus also applies to the export)
    attachStrip(m_profile, *tractor, tl.masterVolumeDb, 0.0, hooks ? &hooks->master : nullptr,
                std::make_pair(tl.masterLimiter, tl.masterLimiterDb));
    return tractor;
}
