// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `index`: the material index (engine/MediaIndex) - build it, search it (core/IndexSearch), write
// descriptions and tags into it
#include "cli/AnalysisDetail.h"

#include "core/ContactSheet.h"
#include "core/PathKey.h"
#include "core/Retime.h"
#include "engine/MediaIndex.h"
#include "engine/Snapshot.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

constexpr int kSheetTiles = 40; // shots shown to the AI at once (MCP)

double r2(double v) { return std::round(v * 100) / 100; }
double r3(double v) { return std::round(v * 1000) / 1000; }

QJsonObject shotJson(const MediaIndex::Shot& s, const QString& thumbFile)
{
    const MediaIndex::Quality& q = s.quality;
    QJsonObject quality{{"sharp", r2(q.sharp)}, {"bright", r2(q.bright)},     {"dark", q.dark},
                        {"overexposed", q.overexposed}, {"blurry", q.blurry}, {"motion", q.motion},
                        {"pan", q.pan},         {"face", q.face}};
    if (q.faces >= 0) quality["faces"] = q.faces; // only measured with the face model (extension "faces")
    if (q.faces > 0) quality["face_box"] = QJsonArray{r3(q.faceX), r3(q.faceY), r3(q.faceW), r3(q.faceH)};
    QJsonObject o{{"n", s.n}, {"from_s", r3(s.from)}, {"to_s", r3(s.to)}, {"thumb_s", r3(s.thumb)}, {"quality", quality}};
    if (!thumbFile.isEmpty()) o["thumb"] = thumbFile;
    if (!s.speech.isEmpty()) o["speech"] = s.speech;
    if (!s.events.isEmpty()) {
        QJsonArray ev;
        for (const AudioEvents::Event& e : s.events)
            ev << QJsonObject{{"kind", e.kind}, {"from_s", r3(e.from)}, {"to_s", r3(e.to)}, {"conf", r2(e.conf)}};
        o["events"] = ev;
    }
    if (!s.tags.isEmpty()) o["tags"] = QJsonArray::fromStringList(s.tags);
    if (!s.desc.isEmpty()) o["desc"] = s.desc;
    return o;
}

// The files to look at: 'files'/'file', else the project's media
struct Material {
    std::unique_ptr<Session> session;
    QStringList files;
};

Material materialOf(const QJsonObject& a)
{
    Material m;
    if (a.contains("project")) {
        m.session = std::make_unique<Session>(a["project"].toString());
        m.session->selectTimeline(a["timeline"]);
    }
    if (a.contains("file")) m.files << absolute(a["file"].toString());
    for (const QJsonValue& v : a["files"].toArray()) m.files << absolute(v.toString());
    if (m.files.isEmpty() && m.session)
        for (const MediaInfo& mi : m.session->project.media())
            if (mi.hasVideo || mi.hasAudio) m.files << mi.path;
    m.files.removeDuplicates();
    return m;
}

// The words of the project's transcripts of a file (all its audio streams)
void addProjectSpeech(const Material& m, MediaIndex::Entry& e)
{
    if (!m.session) return;
    const MediaInfo* mi = m.session->project.mediaInfo(e.file);
    if (!mi) return;
    for (const MediaTranscript& t : mi->transcripts) MediaIndex::addSpeech(e, t.words);
}

QVector<MediaIndex::Entry> entriesOf(const Material& m, bool refresh, bool measure, Context& ctx)
{
    QVector<MediaIndex::Entry> out;
    for (int i = 0; i < m.files.size(); ++i) {
        const QString& file = m.files[i];
        if (ctx.stage) ctx.stage(QFileInfo(file).fileName());
        std::optional<MediaIndex::Entry> e;
        if (!refresh) e = MediaIndex::cached(file);
        if (!e && measure) {
            if (!QFileInfo::exists(file)) fail("NOT_FOUND", "file not found: " + file);
            MediaIndex::Options o;
            o.refresh = refresh;
            const int n = int(m.files.size());
            o.progress = [&ctx, i, n](double p) {
                if (ctx.progress) ctx.progress((i + p) / n);
                return true;
            };
            QString error;
            e = MediaIndex::build(file, o, &error);
            if (!e) fail("DECODE_FAILED", error);
        }
        if (!e) continue;
        addProjectSpeech(m, *e);
        out << *e;
        if (ctx.progress) ctx.progress(double(i + 1) / m.files.size());
    }
    return out;
}

// Timeline frame where clip c plays source frame `source` (as EditOps' timeRef), -1 = not inside the clip
int timelineAt(const Project& p, const Clip& c, int source)
{
    const MediaInfo* m = p.mediaInfo(c.mediaPath);
    const RetimeMap map(c, m ? m->length : 0);
    const int t = c.start + map.materialForPicture(source) - c.in;
    return t >= c.start && t < c.end() ? t : -1;
}

// Where the source range [from, to) seconds of `file` plays on the edited timeline: per clip (video clips, or the
// audio clips of a sound-only file) the part of it that shows the range
QJsonArray placements(const Session& s, const QString& file, double from, double to)
{
    const ProjectFormat f = s.format();
    const Project& p = s.project;
    const Timeline& tl = p.timeline();
    const double fps = f.rate.fps();
    const int srcFrom = int(std::floor(from * fps + 1e-6)), srcTo = std::max(srcFrom + 1, int(std::ceil(to * fps - 1e-6)));
    QJsonArray out;
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<Track>& tracks = kind == TrackKind::Video ? tl.video : tl.audio;
        for (int ti = 0; ti < tracks.size(); ++ti)
            for (const Clip& c : tracks[ti].clips) {
                if (c.kind != ClipKind::Media || !PathKey::same(c.mediaPath, file)) continue;
                const MediaInfo* m = p.mediaInfo(c.mediaPath);
                const RetimeMap map(c, m ? m->length : 0);
                const int a = map.pictureFrame(c.in), b = map.pictureFrame(c.out);
                const int lo = std::max(srcFrom, std::min(a, b)), hi = std::min(srcTo, std::max(a, b) + 1);
                if (hi <= lo) continue;
                const int t0 = timelineAt(p, c, lo), t1 = timelineAt(p, c, hi - 1);
                if (t0 < 0 || t1 < 0) continue;
                QJsonObject o = range(std::min(t0, t1), std::max(t0, t1) + 1, f);
                o["clip"] = c.id;
                o["track"] = trackName({kind, ti});
                out << o;
            }
        if (!out.isEmpty()) break; // the picture's clips; their linked sound would only repeat them
    }
    return out;
}

// Shots as pictures (MCP: the AI looks and writes `annotate`): the middle picture of each, labelled "<file#>.<shot>"
void addSheet(const QVector<MediaIndex::Entry>& entries, bool onlyUndescribed, Context& ctx, QJsonObject& result)
{
    QVector<QImage> images;
    QStringList labels;
    QJsonArray shown;
    for (int fi = 0; fi < entries.size() && images.size() < kSheetTiles; ++fi) {
        const MediaIndex::Entry& e = entries[fi];
        if (!e.video) continue;
        MediaSource ms;
        try {
            ms = openMedia({{"file", e.file}});
        } catch (const Error&) {
            continue;
        }
        const ProjectFormat f = pictureFormat(ms);
        const auto snap = Snapshot::Source::media(f, e.file);
        for (const MediaIndex::Shot& s : e.shots) {
            if (images.size() >= kSheetTiles) break;
            if (onlyUndescribed && !s.desc.isEmpty()) continue;
            const QImage img = snap->frame(std::clamp(int(std::lround(s.thumb * f.rate.fps())), 0, std::max(0, ms.info.length - 1)), 320);
            if (img.isNull()) continue;
            images << img;
            labels << QString("%1.%2").arg(fi + 1).arg(s.n);
            shown << QJsonObject{{"label", labels.back()}, {"file", e.file}, {"shot", s.n}};
        }
    }
    if (images.isEmpty()) return;
    ctx.images << ContactSheet::compose(images, labels, images.size() > 16 ? 6 : 4);
    result["sheet"] = shown;
}

QJsonArray annotateAll(const QJsonArray& list, Context& ctx)
{
    QJsonArray done;
    for (const QJsonValue& v : list) {
        if (!v.isObject()) fail("BAD_ARGUMENT", "annotate: each entry is {file, shot, desc?, tags?}");
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!QStringList{"file", "shot", "desc", "tags"}.contains(it.key()))
                fail("BAD_ARGUMENT", QString("annotate: unknown key '%1' (allowed: file, shot, desc, tags)").arg(it.key()));
        const QString file = absolute(o["file"].toString());
        if (file.isEmpty() || !o["shot"].isDouble()) fail("BAD_ARGUMENT", "annotate: each entry needs 'file' and 'shot'");
        if (!o.contains("desc") && !o.contains("tags")) fail("BAD_ARGUMENT", "annotate: give 'desc' and/or 'tags'");
        std::optional<MediaIndex::Entry> e = MediaIndex::cached(file);
        if (!e) {
            Material m;
            m.files << file;
            const QVector<MediaIndex::Entry> built = entriesOf(m, false, true, ctx);
            if (built.isEmpty()) fail("NOT_FOUND", "file not found: " + file);
            e = built.first();
        }
        std::optional<QString> desc;
        std::optional<QStringList> tags;
        if (o.contains("desc")) desc = o["desc"].toString();
        if (o.contains("tags")) {
            QStringList t;
            const QJsonValue tv = o["tags"];
            if (tv.isString()) t = tv.toString().split(',', Qt::SkipEmptyParts);
            else
                for (const QJsonValue& x : tv.toArray()) t << x.toString();
            tags = t;
        }
        QString error;
        if (!MediaIndex::annotate(*e, o["shot"].toInt(), desc, tags, &error)) fail("BAD_ARGUMENT", error);
        for (const MediaIndex::Shot& s : e->shots)
            if (s.n == o["shot"].toInt())
                done << QJsonObject{{"file", file}, {"shot", s.n}, {"from_s", r3(s.from)}, {"to_s", r3(s.to)},
                                    {"desc", s.desc}, {"tags", QJsonArray::fromStringList(s.tags)}};
    }
    return done;
}

} // namespace

QJsonObject cmdIndex(const QJsonObject& a, Context& ctx)
{
    QJsonObject result;
    if (a.contains("annotate")) {
        if (!a["annotate"].isArray()) fail("BAD_ARGUMENT", "annotate: a list of {file, shot, desc?, tags?}");
        result["annotated"] = annotateAll(a["annotate"].toArray(), ctx);
        if (!a.contains("query")) return result;
    }
    const Material m = materialOf(a);
    if (m.files.isEmpty()) fail("BAD_ARGUMENT", "give 'files' (or 'file'), or a 'project' whose media to index");
    const bool refresh = a["refresh"].toBool();
    const QVector<MediaIndex::Entry> entries = entriesOf(m, refresh, true, ctx);

    if (a.contains("query")) {
        QVector<IndexSearch::Shot> shots;
        for (const MediaIndex::Entry& e : entries) shots += MediaIndex::searchable(e);
        const int top = a.contains("top") ? std::clamp(a["top"].toInt(), 1, 200) : 10;
        QJsonArray hits;
        for (const IndexSearch::Hit& h : IndexSearch::search(a["query"].toString(), shots, top)) {
            QJsonObject o{{"file", h.file}, {"shot", h.n}, {"from_s", r3(h.from)}, {"to_s", r3(h.to)},
                          {"score", h.score}, {"why", h.why}};
            if (m.session) {
                const QJsonArray at = placements(*m.session, h.file, h.from, h.to);
                if (!at.isEmpty()) o["timeline"] = at;
            }
            hits << o;
        }
        result["hits"] = hits;
        result["query_words"] = QJsonArray::fromStringList(IndexSearch::queryWords(a["query"].toString()));
        if (hits.isEmpty())
            result["note"] = "nothing found; descriptions and tags make shots findable (annotate)";
        return result;
    }

    const QString thumbs = a["thumbs"].toString();
    QJsonArray media;
    bool allCached = true;
    int undescribed = 0;
    for (const MediaIndex::Entry& e : entries) {
        allCached = allCached && e.cached;
        std::unique_ptr<Snapshot::Source> snap;
        ProjectFormat f;
        int length = 0;
        if (!thumbs.isEmpty() && e.video) {
            const MediaSource ms = openMedia({{"file", e.file}});
            f = pictureFormat(ms);
            length = ms.info.length;
            snap = Snapshot::Source::media(f, e.file);
            QDir().mkpath(thumbs);
        }
        QJsonArray shots;
        for (const MediaIndex::Shot& s : e.shots) {
            QString thumbFile;
            if (snap) {
                const QImage img = snap->frame(std::clamp(int(std::lround(s.thumb * f.rate.fps())), 0, std::max(0, length - 1)), 320);
                // The name plus a short hash of the whole path: two "clip.mp4" from different folders (or clip.mp4 and
                // clip.mov) overwrote each other's pictures
                const QString tag = QString::fromLatin1(
                    QCryptographicHash::hash(e.file.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
                thumbFile = QDir(thumbs).absoluteFilePath(QString("%1_%2_shot%3.jpg")
                                                              .arg(QFileInfo(e.file).completeBaseName(), tag)
                                                              .arg(s.n, 3, 10, QChar('0')));
                if (img.isNull() || !img.save(thumbFile, "JPG", 85)) thumbFile.clear();
            }
            if (s.desc.isEmpty()) ++undescribed;
            shots << shotJson(s, thumbFile);
        }
        media << QJsonObject{{"file", e.file}, {"duration_s", r3(e.duration)}, {"video", e.video}, {"shots", shots}};
    }
    result["media"] = media;
    result["cached"] = allCached;
    if (ctx.mcp && a["sheet"].toBool(true) && undescribed) addSheet(entries, true, ctx, result);
    if (undescribed)
        result["note"] = QString("%1 shots have no description yet: look at them (sheet, or `frames`) and write "
                                 "{annotate:[{file, shot, desc, tags}]} - then `index` query finds them")
                             .arg(undescribed);
    return result;
}

} // namespace Cli::detail
