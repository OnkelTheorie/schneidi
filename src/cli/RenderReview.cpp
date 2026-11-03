// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: the review render (`render review:"video"|"sheet"`, core/ReviewSheet): collects what to mark from the
// timeline (cuts, accents, notes, markers, story sections) and the analyses (beat grid of the music track via
// core/Beats, hits and kill feed lines of the recordings via the cached `highlights`), and the numbers an AI would
// otherwise have to guess (each hit next to its nearest beat, cuts off the beat). The project is only read.
#include "cli/AnalysisDetail.h"

#include "core/Beats.h"
#include "core/GameHud.h"
#include "core/Retime.h"
#include "core/ReviewSheet.h"
#include "core/TimelineOps.h"
#include "core/TimelineStory.h"
#include "engine/AudioAnalysis.h"
#include "engine/Snapshot.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

namespace Cli::detail {

namespace {

double round3(double v) { return std::round(v * 1000) / 1000; }

// The top visible clip with a picture at timeline frame f (titles left out: they lie over the shot)
const Clip* topClip(const Timeline& tl, int f)
{
    for (int i = tl.video.size() - 1; i >= 0; --i) {
        if (tl.video[i].hidden) continue;
        for (const Clip& c : tl.video[i].clips)
            if (!c.isTitle() && f >= c.start && f < c.end()) return &c;
    }
    return nullptr;
}

QString nameOf(const Project& p, const Clip* c) { return c ? p.clipName(*c) : QString(); }

// Timeline frame where clip c shows source frame `source` (project rate); -1 = not in the clip
int timelineAtSource(const Project& p, const Clip& c, double source)
{
    const MediaInfo* m = p.mediaInfo(c.mediaPath);
    const RetimeMap map(c, m ? m->length : 0);
    const int t = c.start + map.materialForPicture(source) - c.in;
    return t >= c.start && t < c.end() ? t : -1;
}

// The music track: given ("A2", "off"), else the first audio track with sound whose clips are all free (not linked to
// a picture) and from files without video, else the first whose clips are all free
int beatTrackOf(const Project& p, const Timeline& tl, const QJsonValue& v)
{
    if (v.isString() && v.toString().compare("off", Qt::CaseInsensitive) == 0) return -1;
    if (!v.isUndefined() && !v.isNull()) {
        TrackKind k = TrackKind::Audio;
        const int i = parseTrack(v, &k);
        if (k != TrackKind::Audio || i >= tl.audio.size())
            fail("NOT_FOUND", QString("beats_track: there is no audio track %1 (the timeline has A1-A%2)")
                                  .arg(v.isString() ? v.toString() : QString::number(v.toInt()))
                                  .arg(tl.audio.size()));
        return i;
    }
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < tl.audio.size(); ++i) {
            const Track& t = tl.audio[i];
            if (t.muted || t.clips.isEmpty()) continue;
            const bool fits = std::all_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) {
                if (c.linkId != 0) return false;
                if (pass == 1) return true;
                const MediaInfo* m = p.mediaInfo(c.mediaPath);
                return m && !m->hasVideo;
            });
            if (fits) return i;
        }
    return -1;
}

// The sound of one audio track: its waveform (and beat features when asked)
struct TrackSound {
    ReviewSheet::Wave wave;
    std::optional<Beats::Features> features;
    bool beats = false;
    std::vector<float> mono;
    long long samples = 0; // per channel so far
    float peak = 0;
    int perValue = 0; // samples per waveform value
    double fps = 25;
    void add(const float* pcm, int count, int channels, int rate)
    {
        if (perValue <= 0) perValue = std::max(1, rate / ReviewSheet::kWaveRate);
        if (beats && !features) features.emplace(rate);
        if (!pcm) count = int(std::lround(rate / fps));
        mono.assign(size_t(std::max(0, count)), 0.f);
        if (pcm)
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < count; ++i) mono[size_t(i)] += pcm[size_t(c) * size_t(count) + size_t(i)];
        for (float& s : mono) {
            s /= float(std::max(1, channels));
            peak = std::max(peak, std::abs(s));
            if (++samples % perValue == 0) {
                wave.peak.push_back(std::min(1.f, peak));
                peak = 0;
            }
        }
        if (features && !mono.empty()) features->add(mono.data(), int(mono.size()));
    }
};

std::optional<TrackSound> trackSound(const ProjectFormat& f, Timeline tl, int track, int end, bool beats)
{
    for (int i = 0; i < tl.audio.size(); ++i) {
        tl.audio[i].muted = i != track;
        tl.audio[i].solo = false;
    }
    TrackSound s;
    s.beats = beats;
    s.fps = f.rate.fps();
    s.wave.track = trackName({TrackKind::Audio, track});
    if (!AudioAnalysis::timelineSound(f, tl, 0, end,
                                      [&s](const float* pcm, int n, int ch, int rate) { s.add(pcm, n, ch, rate); }))
        return std::nullopt;
    return s;
}

QJsonObject offsetJson(const ReviewSheet::Offset& o, const char* atKey)
{
    QJsonObject j{{atKey, round3(o.at)}, {"beat_s", round3(o.beat)}, {"off_ms", o.offMs}, {"frame", o.frame}};
    if (o.clip) j["clip"] = o.clip;
    if (!o.label.isEmpty()) j["name"] = o.label;
    return j;
}

} // namespace

std::shared_ptr<const ReviewSheet::Data> reviewData(Session& s, const Timeline& tl, int end, const QJsonObject& a,
                                                    Context& ctx, bool allWaves, QJsonObject* info)
{
    using namespace ReviewSheet;
    const Project& p = s.project;
    auto d = std::make_shared<Data>();
    d->format = s.format();
    d->length = end;
    d->title = p.sequenceName(p.currentSequence());
    const double fps = d->fps();
    QJsonArray notes;

    // Cuts: where a clip with a picture starts or ends inside the timeline, named after the clip shown from there on
    std::set<int> cuts;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            if (c.isTitle()) continue;
            if (c.start > 0 && c.start < end) cuts.insert(c.start);
            if (c.end() > 0 && c.end() < end) cuts.insert(c.end());
        }
    }
    for (int f : cuts) {
        const Clip* c = topClip(tl, f);
        d->marks << Mark{MarkKind::Cut, f, c ? nameOf(p, c) : QStringLiteral("—"), c ? c->id : 0};
    }

    // Accents (op 'accent' writes keyframes): a zoom or exposure keyframe above both neighbours
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            std::set<int> at;
            for (AnimParam param : {AnimParam::ZoomX, AnimParam::GradeExposure}) {
                const KeyTrack keys = c.keys.value(param);
                for (int i = 1; i < keys.size(); ++i) {
                    const bool aboveNext = i + 1 >= keys.size() || keys[i].value > keys[i + 1].value;
                    if (keys[i].value > keys[i - 1].value && aboveNext) {
                        const int f = c.start + keys[i].frame - c.in;
                        if (f >= c.start && f < c.end()) at.insert(f);
                    }
                }
            }
            for (int f : at) d->marks << Mark{MarkKind::Accent, f, nameOf(p, &c), c.id};
        }
    }

    // Notes (open ones), markers, story sections
    for (const TimelineNote& n : tl.notes)
        if (!n.done) d->marks << Mark{MarkKind::Note, n.at, n.text, 0};
    for (int m : tl.markers) d->marks << Mark{MarkKind::Marker, m, QString(), 0};
    for (const StorySection& sec : tl.sections)
        d->sections << Section{TimelineStory::kindName(sec.kind), sec.name, sec.from, sec.to};

    // Hits and kill feed lines of the recordings (cached `highlights` per file), where the timeline shows them
    const QJsonValue hitsArg = a["review_hits"];
    const bool hitsOff = (hitsArg.isString() && hitsArg.toString().compare("off", Qt::CaseInsensitive) == 0)
                         || (hitsArg.isBool() && !hitsArg.toBool());
    if (!hitsOff && !hitsArg.isUndefined() && !hitsArg.isNull() && !hitsArg.isObject() && !hitsArg.isBool())
        fail("BAD_ARGUMENT", "review_hits: \"off\" or {game} / {region, color, tolerance?} (the kill feed to read)");
    QJsonArray hitFiles;
    if (!hitsOff) {
        QStringList files;
        for (const Track& t : tl.video) {
            if (t.hidden) continue;
            for (const Clip& c : t.clips)
                if (c.kind == ClipKind::Media && !files.contains(c.mediaPath))
                    if (const MediaInfo* m = p.mediaInfo(c.mediaPath); m && m->hasVideo && !m->isImage)
                        files << c.mediaPath;
        }
        for (const QString& file : files) {
            QJsonObject args{{"file", file}};
            for (const char* k : {"game", "region", "color", "tolerance", "min_area", "audio_stream"})
                if (hitsArg.toObject().contains(k)) args[k] = hitsArg.toObject()[k];
            QJsonObject fileInfo{{"file", file}};
            if (!args.contains("game") && !args.contains("color") && !GameHud::guess(file)) {
                fileInfo["skipped"] = "no kill feed known for this file: review_hits {game} or {region, color}";
                hitFiles << fileInfo;
                continue;
            }
            QJsonObject r;
            try {
                Context quiet; // the review's own progress is not split into the analysis
                r = highlightsFile(args, quiet);
            } catch (const Error& e) {
                fileInfo["skipped"] = e.code + ": " + e.message;
                hitFiles << fileInfo;
                continue;
            }
            if (r.contains("game")) fileInfo["game"] = r["game"];
            if (r["cached"].toBool()) fileInfo["cached"] = true;
            int hits = 0, kills = 0;
            std::set<std::pair<int, int>> seen; // (kind, frame)
            for (const QJsonValue& cv : r["candidates"].toArray())
                for (const auto& [key, kind] : {std::pair{"hit_s", MarkKind::Hit}, std::pair{"kill_s", MarkKind::Kill}})
                    for (const QJsonValue& sv : cv[QLatin1String(key)].toArray()) {
                        const double source = sv.toDouble() * fps;
                        for (const Track& t : tl.video) {
                            if (t.hidden) continue;
                            for (const Clip& c : t.clips) {
                                if (c.kind != ClipKind::Media || c.mediaPath != file) continue;
                                const int f = timelineAtSource(p, c, source);
                                if (f < 0 || f >= end || !seen.insert({int(kind), f}).second) continue;
                                d->marks << Mark{kind, f, nameOf(p, &c), c.id};
                                (kind == MarkKind::Hit ? hits : kills)++;
                            }
                        }
                    }
            fileInfo["hits"] = hits;
            fileInfo["kills"] = kills;
            hitFiles << fileInfo;
        }
    }
    if (info && !hitFiles.isEmpty()) (*info)["hits_from"] = hitFiles;

    // Beat grid of the music track; waveforms (the music track's, or every audio track's for the sheet)
    const int beatTrack = beatTrackOf(p, tl, a["beats_track"]);
    for (int i = 0; i < tl.audio.size(); ++i) {
        const bool beats = i == beatTrack;
        if (!beats && (!allWaves || tl.audio[i].clips.isEmpty())) continue;
        if (ctx.stage) ctx.stage(beats ? "beats" : "waveform");
        std::optional<TrackSound> sound = trackSound(d->format, tl, i, end, beats);
        if (!sound) {
            notes << QString("could not mix the sound of %1").arg(trackName({TrackKind::Audio, i}));
            continue;
        }
        if (beats && sound->features) {
            const Beats::Result r = Beats::analyze(sound->features->take());
            if (r.found) {
                d->beatTrack = sound->wave.track;
                d->bpm = r.bpm;
                for (int k = 0; k < r.beats; ++k) {
                    d->beats.push_back(Beats::beatTime(r, k));
                    d->bar.push_back(char(((k - r.downbeat) % 4 + 4) % 4 == 0));
                }
                if (info)
                    (*info)["beats"] = QJsonObject{{"track", d->beatTrack}, {"bpm", std::round(r.bpm * 100) / 100},
                                                   {"confidence", std::round(r.confidence * 100) / 100},
                                                   {"count", r.beats}, {"first_s", round3(d->beats.empty() ? 0 : d->beats.front())}};
            } else {
                notes << QString("no beat found on %1 (beats_track: the music track)").arg(sound->wave.track);
            }
        }
        if (beats) d->musicWave = int(d->waves.size());
        d->waves << sound->wave;
    }
    if (beatTrack < 0) notes << "no music track for the beat grid (every audio track has linked or video sound): pass beats_track";
    d->sort();
    if (info && !notes.isEmpty()) (*info)["review_notes"] = notes;
    return d;
}

QJsonObject reviewNumbers(const ReviewSheet::Data& d)
{
    using namespace ReviewSheet;
    QJsonObject o;
    QJsonArray offs, cuts;
    for (const Offset& x : offsets(d, MarkKind::Hit)) {
        QJsonObject j = offsetJson(x, "hit_s");
        offs << j;
    }
    for (const Offset& x : cutsOffBeat(d)) cuts << offsetJson(x, "at_s");
    o["offsets"] = offs;
    o["cuts_off_beat"] = cuts;
    int counts[6] = {};
    for (const Mark& m : d.marks) ++counts[int(m.kind)];
    o["marks"] = QJsonObject{{"cuts", counts[int(MarkKind::Cut)]},       {"hits", counts[int(MarkKind::Hit)]},
                             {"kills", counts[int(MarkKind::Kill)]},     {"accents", counts[int(MarkKind::Accent)]},
                             {"notes", counts[int(MarkKind::Note)]},     {"markers", counts[int(MarkKind::Marker)]},
                             {"sections", int(d.sections.size())},       {"beats", int(d.beats.size())}};
    o["offsets_note"] = "offsets: each hit (hit_s, timeline seconds) next to the nearest beat of the music track "
                        "(off_ms > 0 = after the beat); cuts_off_beat: cuts more than half a frame from their nearest "
                        "beat (moving them by off_ms puts them on it)";
    return o;
}

QJsonObject reviewSheetRender(Session& s, const Timeline& tl, const ReviewSheet::Data& d, const QString& out,
                              Context& ctx)
{
    using namespace ReviewSheet;
    QVector<Tile> tiles = shots(d);
    std::unique_ptr<Snapshot::Source> src = Snapshot::Source::timeline(s.format(), tl);
    for (int i = 0; i < tiles.size(); ++i) {
        if (ctx.progress) ctx.progress(0.5 + 0.45 * i / std::max<qsizetype>(1, tiles.size()));
        Tile& t = tiles[i];
        if (src) t.image = src->frame(t.frame, 360);
        t.clip = nameOf(s.project, topClip(tl, t.frame));
    }
    const QImage img = sheet(d, tiles);
    QDir().mkpath(QFileInfo(out).absolutePath());
    if (img.isNull() || !img.save(out)) fail("WRITE_FAILED", "cannot write " + out);
    if (ctx.mcp) ctx.images << img;
    QJsonArray list;
    const ProjectFormat f = s.format();
    for (const Tile& t : tiles)
        list << QJsonObject{{"from_s", seconds(t.from, f)},
                            {"to_s", seconds(t.to, f)},
                            {"frame", t.frame},
                            {"clip", t.clip},
                            {"rect", QJsonArray{t.rect.x(), t.rect.y(), t.rect.width(), t.rect.height()}}};
    return QJsonObject{{"out", out}, {"size", QJsonArray{img.width(), img.height()}}, {"tiles", list}};
}

} // namespace Cli::detail
