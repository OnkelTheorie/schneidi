// schneidi-cli: commands that look at the material (probe, silence, scenes, frames) without changing a project
#include "cli/CommandsDetail.h"

#include "core/I18n.h"
#include "core/Loudness.h"
#include "core/ProjectFile.h"
#include "core/Subtitles.h"
#include "core/Transcript.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Bundle.h"
#include "engine/Engine.h"
#include "engine/Exporter.h"
#include "engine/Extensions.h"
#include "engine/Transcriber.h"
#include "engine/Profiles.h"
#include "engine/Snapshot.h"
#include "ui/Scopes.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>
#include <tuple>
#include <cmath>
#include <memory>

namespace Cli::detail {

namespace {
// Media file + the frame rate frames count in (project format, or the file's own rate)
struct MediaSource {
    ProjectFormat format;
    MediaInfo info;
    ClipFormat clip; // the file's own format (detected when no project gives the frame rate)
};

// Project format read once for several files looked at in parallel (perFile), instead of each thread opening
// the project (or the running app's)
const ProjectFormat* g_projectFormat = nullptr;

MediaSource openMedia(const QJsonObject& a)
{
    const QString path = a["file"].toString();
    if (!QFileInfo::exists(path)) fail("NOT_FOUND", "file not found: " + path);
    MediaSource m;
    if (a.contains("project") && g_projectFormat) {
        m.format = *g_projectFormat;
    } else if (a.contains("project")) {
        Session s(a["project"].toString());
        m.format = s.format();
    } else {
        m.clip = detectClipFormat(path);
        const ClipFormat& cf = m.clip;
        if (cf.ok) {
            m.format.rate = cf.suggested;
            m.format.width = cf.width & ~1;
            m.format.height = cf.height & ~1;
        }
    }
    m.info = Engine::probe(m.format, path);
    if (m.info.length <= 0 || (!m.info.hasVideo && !m.info.hasAudio)) fail("BAD_MEDIA", "cannot read media: " + path);
    return m;
}

QImage sheet(const QVector<QImage>& images, const QStringList& labels)
{
    const int cols = std::min<int>(images.size(), images.size() <= 4 ? 2 : images.size() <= 9 ? 3 : 4);
    const int rows = (images.size() + cols - 1) / cols;
    const QSize cell = images.first().size();
    QImage out(cols * cell.width(), rows * cell.height(), QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    QFont font = p.font();
    font.setPixelSize(std::max(12, cell.height() / 14));
    p.setFont(font);
    for (int i = 0; i < images.size(); ++i) {
        const QPoint at((i % cols) * cell.width(), (i / cols) * cell.height());
        p.drawImage(QRect(at, cell), images[i]); // several sources may differ in size
        const QRect box(at + QPoint(4, 4), QSize(p.fontMetrics().horizontalAdvance(labels[i]) + 8, p.fontMetrics().height() + 2));
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, labels[i]);
        p.setPen(QColor(60, 60, 60));
        p.drawRect(QRect(at, cell).adjusted(0, 0, -1, -1));
    }
    return out;
}

// Pauses in per-frame peaks: quiet runs of at least minLen frames; the pad of sound stays on both sides (not at
// the start/end). Ranges are shifted by `offset`.
QJsonObject silenceJson(const std::vector<float>& peaks, int offset, double thresholdDb, int minLen, int pad,
                        const ProjectFormat& f)
{
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    const int n = int(peaks.size());
    QJsonArray silence, sound;
    int keepFrom = 0, silent = 0;
    for (int i = 0; i <= n; ++i) {
        if (i < n && peaks[i] < limit) {
            ++silent;
            continue;
        }
        if (silent >= minLen) {
            const int from = i - silent, to = i;
            const int cutFrom = from == 0 ? 0 : std::min(to, from + pad);
            const int cutTo = to == n ? n : std::max(cutFrom, to - pad);
            if (cutTo > cutFrom) {
                silence << range(offset + cutFrom, offset + cutTo, f);
                if (cutFrom > keepFrom) sound << range(offset + keepFrom, offset + cutFrom, f);
                keepFrom = cutTo;
            }
        }
        silent = 0;
    }
    if (keepFrom < n) sound << range(offset + keepFrom, offset + n, f);
    int kept = 0;
    for (const QJsonValue& r : sound) kept += r["to"].toInt() - r["from"].toInt();
    return {{"frames", n}, {"fps", formatJson(f)["fps"]}, {"threshold_db", thresholdDb}, {"silence", silence},
            {"sound", sound}, {"kept_frames", kept}, {"kept_s", seconds(kept, f)}, {"removed_s", seconds(n - kept, f)}};
}

} // namespace

// Audio tracks of `tl`: "A3", "A2,A5", numbers, track names or the audio stream names of their clips
// ("Discord,Mikrofon", as `probe` lists them); empty = none given
QVector<int> audioTracksOf(const Project& p, const Timeline& tl, const QJsonValue& list)
{
    static const QRegularExpression trackRef("^[AaVv]?\\d+$");
    QVector<int> out;
    for (QString t : list.toVariant().toString().split(',', Qt::SkipEmptyParts)) {
        t = t.trimmed();
        int index = -1;
        if (trackRef.match(t).hasMatch()) {
            TrackKind k;
            index = parseTrack(t, &k);
            if (k != TrackKind::Audio || index >= tl.audio.size()) fail("BAD_ARGUMENT", "no audio track " + t);
        }
        QStringList names;
        for (int i = 0; i < tl.audio.size(); ++i) {
            const Track& track = tl.audio[i];
            QStringList own{track.name};
            for (const Clip& c : track.clips)
                if (const MediaInfo* m = p.mediaInfo(c.mediaPath)) own << m->audioStreamName(c.audioStream);
            own.removeAll(QString());
            own.removeDuplicates();
            if (index < 0 && own.contains(t, Qt::CaseInsensitive)) index = i;
            if (!own.isEmpty()) names << QString("A%1 = %2").arg(i + 1).arg(own.join("/"));
        }
        if (index < 0)
            fail("BAD_ARGUMENT", QString("no audio track '%1' (tracks: %2)").arg(t, names.isEmpty() ? "none named" : names.join(", ")));
        if (!out.contains(index)) out << index;
    }
    return out;
}

namespace {
QJsonObject probeOne(const QJsonObject& a)
{
    const MediaSource m = openMedia(a);
    // Detecting opens the file and runs ffprobe: only once (with a project openMedia did not need it)
    const ClipFormat cf = a.contains("project") ? detectClipFormat(m.info.path) : m.clip;
    QJsonObject o{{"file", m.info.path}, {"frames", m.info.length}, {"seconds", seconds(m.info.length, m.format)},
                  {"duration_tc", tc(m.info.length, m.format)}, {"video", m.info.hasVideo}, {"audio", m.info.hasAudio},
                  {"frames_count_in", formatJson(m.format)["fps"]}};
    if (m.info.isImage) o["image"] = true;
    if (cf.ok) {
        o["width"] = cf.width;
        o["height"] = cf.height;
        o["fps"] = std::round(cf.rate.fps() * 1000) / 1000;
        if (cf.variable) o["variable_frame_rate"] = true;
    }
    if (m.info.hasAudio) {
        QJsonArray streams;
        for (int i = 0; i < m.info.audioStreamCount(); ++i) streams << m.info.audioStreamName(i);
        o["audio_streams"] = streams;
    }
    return o;
}

QJsonObject probeFile(const QJsonObject& a, Context&) { return probeOne(a); }
} // namespace

QJsonObject cmdSilence(const QJsonObject& a, Context& ctx)
{
    const auto progress = [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    };
    const auto settings = [&a](const ProjectFormat& f) {
        const double thresholdDb = a.contains("threshold_db") ? a["threshold_db"].toDouble() : -35.0;
        const int minLen = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));
        const int pad = a.contains("pad") ? parseTime(a["pad"], f, "pad") : int(std::lround(0.1 * f.rate.fps()));
        return std::tuple{thresholdDb, minLen, pad};
    };
    const QString file = a["file"].toString();
    if (isProjectFile(file)) {
        // The timeline's sound as exported (optionally some tracks); ranges in timeline frames, ready for keep
        Session s(file);
        s.selectTimeline(a["timeline"]);
        const ProjectFormat& f = s.format();
        Timeline tl = s.project.renderTimeline();
        const int end = TimelineOps::endFrame(tl);
        if (end <= 0) fail("EMPTY", "the timeline is empty");
        const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        const QVector<int> only = audioTracksOf(s.project, tl, a["tracks"]);
        if (!only.isEmpty())
            for (int i = 0; i < tl.audio.size(); ++i) {
                tl.audio[i].muted = !only.contains(i);
                tl.audio[i].solo = false;
            }
        const auto peaks = AudioAnalysis::timelineFramePeaks(f, tl, from, to, progress);
        if (!peaks) fail("DECODE_FAILED", "could not mix the sound");
        const auto [thresholdDb, minLen, pad] = settings(f);
        QJsonObject o{{"project", s.path.isEmpty() ? absolute(file) : s.path}, {"range", range(from, to, f)}};
        const QJsonObject r = silenceJson(*peaks, from, thresholdDb, minLen, pad, f);
        for (auto it = r.begin(); it != r.end(); ++it) o[it.key()] = it.value();
        return o;
    }
    const MediaSource m = openMedia(a);
    if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
    const ProjectFormat& f = m.format;
    Clip c;
    c.mediaPath = m.info.path;
    c.out = m.info.length - 1;
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c, progress);
    if (!peaks) fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
    const auto [thresholdDb, minLen, pad] = settings(f);
    QJsonObject o{{"file", m.info.path}};
    const QJsonObject r = silenceJson(*peaks, 0, thresholdDb, minLen, pad, f);
    for (auto it = r.begin(); it != r.end(); ++it) o[it.key()] = it.value();
    return o;
}


namespace {

// "x,y,w,h" in pixels of the full picture, or fractions of it when all four are <= 1 ("0.75,0,0.25,0.3")
QRect cropRect(const QString& spec, QSize full)
{
    const QStringList parts = spec.split(',');
    QVector<double> v;
    for (const QString& p : parts) {
        bool ok = false;
        v << QLocale::c().toDouble(p.trimmed(), &ok);
        if (!ok) v.clear();
        if (v.isEmpty()) break;
    }
    if (v.size() != 4) fail("BAD_ARGUMENT", "crop must be \"x,y,w,h\" (pixels, or fractions 0..1 of the picture)");
    if (std::all_of(v.begin(), v.end(), [](double x) { return x >= 0 && x <= 1; }) && (v[2] < 1 || v[3] < 1))
        v = {v[0] * full.width(), v[1] * full.height(), v[2] * full.width(), v[3] * full.height()};
    const QRect r = QRect(int(std::lround(v[0])), int(std::lround(v[1])), int(std::lround(v[2])), int(std::lround(v[3])))
                        .intersected(QRect(QPoint(0, 0), full));
    if (r.width() < 4 || r.height() < 4)
        fail("BAD_ARGUMENT", QString("crop is outside the picture (%1x%2)").arg(full.width()).arg(full.height()));
    return r;
}

// The frames of one source (media file or project timeline) for `frames`
struct FrameSet {
    QString source;
    QVector<QImage> images;
    QStringList labels;
    QJsonArray frames;
};

FrameSet framesOf(const QString& src, const QJsonObject& a, Context& ctx, int maxEdge, const QString& labelPrefix)
{
    std::unique_ptr<Session> session;
    MediaSource media;
    ProjectFormat f;
    int length = 0;
    if (isProjectFile(src)) {
        session = std::make_unique<Session>(src);
        session->selectTimeline(a["timeline"]);
        f = session->format();
        length = TimelineOps::endFrame(session->project.timeline());
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        media = openMedia(ma);
        f = media.format;
        length = media.info.length;
    }
    if (length <= 0) fail("EMPTY", "nothing to show (empty timeline): " + src);
    QVector<int> at;
    for (const QJsonValue& v : a["at"].toArray()) at << parseTime(v, f, "at");
    const int from = a.contains("from") ? std::clamp(parseTime(a["from"], f, "from"), 0, length - 1) : 0;
    const int to = a.contains("to") ? std::clamp(parseTime(a["to"], f, "to"), from + 1, length) : length;
    if (a.contains("every")) {
        const int step = parseTime(a["every"], f, "every");
        if (step <= 0) fail("BAD_ARGUMENT", "'every' must be > 0");
        for (int fr = from; fr < to; fr += step) at << fr;
    }
    if (a.contains("count")) {
        const int n = std::clamp(a["count"].toInt(), 1, 100);
        for (int i = 0; i < n; ++i) at << from + int((i + 0.5) * (to - from) / n);
    }
    if (at.isEmpty()) fail("BAD_ARGUMENT", "give 'at' (times), 'every' (interval) or 'count' (evenly spread)");
    if (at.size() > 100) fail("BAD_ARGUMENT", QString("too many frames (%1, max 100)").arg(at.size()));
    // Crop: decode the full picture, cut the region out, scale it down only if it is larger than maxEdge
    const bool crop = a.contains("crop");
    const QSize full = f.size();
    const QRect region = crop ? cropRect(a["crop"].toString(), full) : QRect();
    const int decodeEdge = crop ? std::max(full.width(), full.height()) : maxEdge;

    const auto snap = session ? Snapshot::Source::timeline(f, session->project.renderTimeline())
                              : Snapshot::Source::media(f, media.info.path);
    FrameSet out;
    out.source = absolute(src);
    for (int i = 0; i < at.size(); ++i) {
        const int fr = std::clamp(at[i], 0, length - 1);
        QImage img = snap->frame(fr, decodeEdge);
        if (img.isNull()) fail("DECODE_FAILED", QString("could not decode frame %1 of %2").arg(fr).arg(src));
        if (crop) {
            // The snapshot may come out smaller than the format (odd sizes): scale the region along
            const double kx = double(img.width()) / full.width(), ky = double(img.height()) / full.height();
            img = img.copy(QRect(int(region.x() * kx), int(region.y() * ky), int(region.width() * kx), int(region.height() * ky)));
            if (std::max(img.width(), img.height()) > maxEdge)
                img = img.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        out.images << img;
        out.labels << labelPrefix + tc(fr, f);
        out.frames << QJsonObject{{"frame", fr}, {"tc", tc(fr, f)}, {"s", seconds(fr, f)}};
        if (ctx.progress) ctx.progress(double(i + 1) / at.size());
    }
    return out;
}

} // namespace

namespace {

// Events in one region of the picture (kill feed, score, HUD): moments where its content changes and then stays.
// The game behind such overlays moves all the time, so a plain frame difference fires constantly; a pixel only
// counts when it was steady just before, differs afterwards and is steady again (a new line that appeared).
QJsonObject regionChanges(const MediaSource& m, const QJsonObject& a, Context& ctx)
{
    const ProjectFormat& f = m.format;
    const QSize full(std::max(2, f.width), std::max(2, f.height));
    const QRect r = cropRect(a["region"].toString(), full);
    constexpr int kRate = 10; // samples per second
    const int w = 128, h = std::clamp(int(std::lround(128.0 * r.height() / r.width())) & ~1, 8, 256);
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "error", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     QString("setpts=PTS-STARTPTS,fps=%1,crop=w=iw*%2:h=ih*%3:x=iw*%4:y=ih*%5,scale=%6:%7,format=rgb24")
                         .arg(kRate)
                         .arg(QString::number(double(r.width()) / full.width(), 'f', 5),
                              QString::number(double(r.height()) / full.height(), 'f', 5),
                              QString::number(double(r.x()) / full.width(), 'f', 5),
                              QString::number(double(r.y()) / full.height(), 'f', 5))
                         .arg(w)
                         .arg(h),
                     "-f", "rawvideo", "-"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed for region events)");
    const qsizetype frameBytes = qsizetype(w) * h * 3;
    const double total = m.info.length / f.rate.fps() * kRate;
    QByteArray raw;
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        raw += ff.readAllStandardOutput();
        if (ctx.progress && total > 0) ctx.progress(std::clamp(raw.size() / frameBytes / total, 0.0, 1.0));
    }
    raw += ff.readAllStandardOutput();
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video: " + QString::fromUtf8(ff.readAllStandardError().right(400)).trimmed());
    const int n = int(raw.size() / frameBytes);
    const auto px = [&](int frame, int i) { return reinterpret_cast<const uchar*>(raw.constData()) + frame * frameBytes + i * 3; };
    const auto diff = [](const uchar* x, const uchar* y) {
        return std::max({std::abs(x[0] - y[0]), std::abs(x[1] - y[1]), std::abs(x[2] - y[2])});
    };
    constexpr int kSteady = 12, kChanged = 40;
    const double sensitivity = a.contains("threshold") ? a["threshold"].toDouble() : 0.3;
    if (sensitivity <= 0 || sensitivity >= 1) fail("BAD_ARGUMENT", "threshold must be between 0 and 1 (default 0.3)");
    const double minShare = 0.01 + sensitivity * 0.1; // share of the region that must change (default 4 %)
    // score(t): share of pixels steady over t-2..t-1, changed from t-1 to t+2 (overlays slide in for a few
    // frames), steady over t+2..t+3
    std::vector<double> score(size_t(std::max(n, 0)), 0.0);
    for (int t = 2; t + 3 < n; ++t) {
        int hits = 0;
        for (int i = 0; i < w * h; ++i)
            if (diff(px(t - 2, i), px(t - 1, i)) < kSteady && diff(px(t + 2, i), px(t + 3, i)) < kSteady
                && diff(px(t - 1, i), px(t + 2, i)) > kChanged)
                ++hits;
        score[size_t(t)] = double(hits) / (w * h);
    }
    const int minGap = std::max(1, int(std::lround((a.contains("min") ? parseTime(a["min"], f, "min") / f.rate.fps() : 0.5) * kRate)));
    QJsonArray changes;
    int last = -minGap;
    for (int t = 2; t + 3 < n; ++t) {
        if (score[size_t(t)] < minShare) continue;
        // the strongest sample of a burst
        bool peak = true;
        for (int k = std::max(0, t - minGap + 1); k < std::min(n, t + minGap) && peak; ++k)
            if (score[size_t(k)] > score[size_t(t)]) peak = false;
        if (!peak || t - last < minGap) continue;
        last = t;
        const int frame = std::min(m.info.length - 1, int(std::lround(double(t) / kRate * f.rate.fps())));
        changes << QJsonObject{{"frame", frame}, {"s", seconds(frame, f)}, {"tc", tc(frame, f)},
                               {"changed_pct", std::round(score[size_t(t)] * 1000) / 10}};
    }
    return {{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]}, {"region", a["region"]},
            {"threshold", sensitivity}, {"changes", changes},
            {"note", "changes = the region's content changed and stayed (a line appeared or went away); check them "
                     "with `frames` crop"}};
}

} // namespace

namespace {
QJsonObject scenesOne(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    if (!m.info.hasVideo || m.info.isImage) fail("NO_VIDEO", "file has no moving picture: " + m.info.path);
    if (a.contains("region")) return regionChanges(m, a, ctx);
    const ProjectFormat& f = m.format;
    const double threshold = a.contains("threshold") ? a["threshold"].toDouble() : 0.3;
    if (threshold <= 0 || threshold >= 1) fail("BAD_ARGUMENT", "threshold must be between 0 and 1 (default 0.3)");
    const int minLen = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));

    // ffmpeg's scene score (0..1, difference to the previous frame) on a small copy of each frame; metadata=print
    // logs time and score of every frame above the threshold. Times start at 0 like the frames in schneidi.
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     QString("setpts=PTS-STARTPTS,scale=320:-2,select='gt(scene,%1)',metadata=mode=print").arg(threshold),
                     "-f", "null", "-", "-progress", "pipe:1"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed for scene detection)");
    const double total = m.info.length / f.rate.fps();
    QByteArray log;
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        log += ff.readAllStandardError();
        const QList<QByteArray> lines = ff.readAllStandardOutput().split('\n');
        for (const QByteArray& line : lines)
            if (line.startsWith("out_time_us=") && ctx.progress && total > 0)
                ctx.progress(std::clamp(line.mid(12).toDouble() / 1e6 / total, 0.0, 1.0));
    }
    log += ff.readAllStandardError();
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video: " + QString::fromUtf8(log.right(400)).trimmed());

    // "frame:12 pts:12 pts_time:0.48" followed by "lavfi.scene_score=0.62"
    static const QRegularExpression timeRe("pts_time:([0-9.]+)"), scoreRe("lavfi\\.scene_score=([0-9.]+)");
    QVector<QPair<int, double>> found;
    for (const QString& line : QString::fromUtf8(log).split('\n')) {
        if (const auto t = timeRe.match(line); t.hasMatch())
            found.append({int(std::lround(t.captured(1).toDouble() * f.rate.fps())), 0.0});
        else if (const auto sc = scoreRe.match(line); sc.hasMatch() && !found.isEmpty())
            found.last().second = sc.captured(1).toDouble();
    }
    // Cuts closer than minLen to the previous one (flashes, fast pans) are dropped; no cut at the very start
    QJsonArray cuts, scenes;
    int sceneStart = 0;
    for (const auto& [frame, score] : found) {
        if (frame <= 0 || frame >= m.info.length || frame - sceneStart < minLen) continue;
        cuts << QJsonObject{{"frame", frame}, {"s", seconds(frame, f)}, {"tc", tc(frame, f)},
                            {"score", std::round(score * 1000) / 1000}};
        scenes << range(sceneStart, frame, f);
        sceneStart = frame;
    }
    scenes << range(sceneStart, m.info.length, f);
    return {{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]}, {"threshold", threshold},
            {"cuts", cuts}, {"scenes", scenes}};
}

// Loud moments of one file: local maxima of the per-frame peak above the threshold, at least `min` apart
QJsonObject peaksOne(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
    const ProjectFormat& f = m.format;
    Clip c;
    c.mediaPath = m.info.path;
    c.out = m.info.length - 1;
    c.audioStream = a["audio_stream"].toInt();
    if (c.audioStream >= m.info.audioStreamCount())
        fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s)").arg(m.info.audioStreamCount()));
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    });
    if (!peaks || peaks->empty()) fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
    std::vector<double> db(peaks->size());
    for (size_t i = 0; i < db.size(); ++i) db[i] = 20 * std::log10(std::max(1e-6f, (*peaks)[i]));
    // Default threshold: well above the file's typical level (median), so it adapts to quiet recordings
    std::vector<double> sorted = db;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = sorted[sorted.size() / 2];
    const double threshold = a.contains("threshold_db") ? a["threshold_db"].toDouble() : std::min(-1.0, median + 12);
    const int minGap = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));
    struct Hit {
        int frame;
        double db;
    };
    QVector<Hit> hits;
    for (int i = 0; i < int(db.size()); ++i) {
        if (db[i] < threshold) continue;
        if (!hits.isEmpty() && i - hits.last().frame < minGap) {
            if (db[i] > hits.last().db) hits.last() = {i, db[i]}; // the loudest frame of a burst counts
            continue;
        }
        hits << Hit{i, db[i]};
    }
    if (a.contains("top")) {
        const int top = std::max(1, a["top"].toInt());
        std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.db > y.db; });
        if (hits.size() > top) hits.resize(top);
        std::sort(hits.begin(), hits.end(), [](const Hit& x, const Hit& y) { return x.frame < y.frame; });
    }
    QJsonArray list;
    for (const Hit& h : hits)
        list << QJsonObject{{"frame", h.frame}, {"s", seconds(h.frame, f)}, {"tc", tc(h.frame, f)},
                            {"peak_db", std::round(h.db * 10) / 10}};
    return {{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]},
            {"median_db", std::round(median * 10) / 10}, {"threshold_db", std::round(threshold * 10) / 10},
            {"peaks", list}};
}

// One file (`file`) or several (`files`, one result each; a failing file gets its error). Several files are
// looked at in parallel, up to `parallel` at once (opening a file and ffprobe take most of the time, mostly
// waiting; ffmpeg decoding for scenes uses all cores itself).
QJsonObject perFile(const QJsonObject& a, Context& ctx, QJsonObject (*one)(const QJsonObject&, Context&),
                    int parallel = 6)
{
    QStringList files;
    if (a.contains("file")) files << a["file"].toString();
    for (const QJsonValue& v : a["files"].toArray()) files << v.toString();
    if (files.isEmpty()) fail("BAD_ARGUMENT", "give 'file' (or 'files', several)");
    if (files.size() == 1 && !a.contains("files")) return one(a, ctx);
    std::optional<ProjectFormat> format;
    if (a.contains("project")) format = Session(a["project"].toString()).format();
    const int n = files.size();
    std::vector<QJsonObject> results(n);
    std::vector<std::atomic<double>> progress(n);
    std::atomic<int> next{0};
    auto work = [&] {
        for (int i; (i = next++) < n;) {
            QJsonObject args = a;
            args.remove("files");
            args["file"] = files[i];
            Context sub;
            sub.progress = [&progress, i](double p) { progress[i] = p; };
            try {
                results[i] = one(args, sub);
            } catch (const Error& e) {
                results[i] = {{"file", files[i]}, {"error", QJsonObject{{"code", e.code}, {"message", e.message}}}};
            } catch (const std::exception& e) {
                results[i] = {{"file", files[i]}, {"error", QJsonObject{{"code", "FAILED"}, {"message", e.what()}}}};
            }
            progress[i] = 1.0;
        }
    };
    g_projectFormat = format ? &*format : nullptr;
    const int threads = std::clamp(QThread::idealThreadCount() / 2, 1, std::min(n, parallel));
    std::atomic<int> running{threads};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            work();
            --running;
        });
    // Progress from this thread only (MCP notifications are written here)
    while (running > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (ctx.progress) {
            double sum = 0;
            for (const std::atomic<double>& p : progress) sum += p;
            ctx.progress(sum / n);
        }
    }
    for (std::thread& t : pool) t.join();
    g_projectFormat = nullptr;
    QJsonArray list;
    for (QJsonObject& r : results) list << r;
    return {{"files", list}};
}
} // namespace

QJsonObject cmdProbe(const QJsonObject& a, Context& ctx) { return perFile(a, ctx, probeFile); }
QJsonObject cmdScenes(const QJsonObject& a, Context& ctx) { return perFile(a, ctx, scenesOne, 2); }
QJsonObject cmdPeaks(const QJsonObject& a, Context& ctx) { return perFile(a, ctx, peaksOne); }



QJsonObject cmdFrames(const QJsonObject& a, Context& ctx)
{
    QStringList sources;
    if (a.contains("source")) sources << a["source"].toString();
    for (const QJsonValue& v : a["sources"].toArray()) sources << v.toString();
    if (sources.isEmpty()) fail("BAD_ARGUMENT", "give 'source' (or 'sources', several files)");
    const int maxEdge = a.contains("width") ? std::clamp(a["width"].toInt(), 64, 3840) : (ctx.mcp ? 480 : 640);
    const QString outDir = a["out"].toString();
    const bool asSheet = a["sheet"].toBool();
    if (outDir.isEmpty() && !ctx.mcp) fail("BAD_ARGUMENT", "missing 'out' (folder for the images, or the sheet file with --sheet)");

    QVector<QImage> images;
    QStringList labels;
    QJsonArray perSource;
    for (int si = 0; si < sources.size(); ++si) {
        Context one;
        one.mcp = ctx.mcp;
        if (ctx.progress) one.progress = [&ctx, si, n = sources.size()](double p) { ctx.progress((si + p) / n); };
        // Several sources on one sheet: label with the source's number (the answer lists them in order)
        FrameSet set = framesOf(sources[si], a, one, maxEdge, sources.size() > 1 ? QString("%1  ").arg(si + 1) : QString());
        for (int i = 0; i < set.images.size(); ++i) {
            if (!asSheet && !outDir.isEmpty()) {
                QDir().mkpath(outDir);
                const QString prefix = sources.size() > 1 ? QString("src%1_").arg(si + 1) : QString();
                const QString file = QDir(outDir).absoluteFilePath(
                    QString("%1frame_%2.jpg").arg(prefix).arg(set.frames[i].toObject()["frame"].toInt(), 6, 10, QChar('0')));
                if (!set.images[i].save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
                QJsonObject o = set.frames[i].toObject();
                o["file"] = file;
                set.frames[i] = o;
            }
        }
        images += set.images;
        labels += set.labels;
        if (images.size() > (asSheet ? 100 : 200))
            fail("BAD_ARGUMENT", QString("too many frames in total (max %1)").arg(asSheet ? 100 : 200));
        perSource << QJsonObject{{"n", si + 1}, {"source", set.source}, {"frames", set.frames}};
    }
    QJsonObject result;
    if (sources.size() == 1) result = {{"source", perSource[0].toObject()["source"]}, {"frames", perSource[0].toObject()["frames"]}};
    else result = {{"sources", perSource}};
    if (asSheet) {
        const QImage s = sheet(images, labels);
        if (!outDir.isEmpty()) {
            QString file = absolute(outDir);
            if (QFileInfo(file).isDir()) file = QDir(file).filePath("sheet.jpg");
            if (!s.save(file, "JPG", 85)) fail("WRITE_FAILED", "cannot write " + file);
            result["sheet"] = file;
        }
        if (ctx.mcp) ctx.images = {s};
    } else if (ctx.mcp) {
        ctx.images = images;
    }
    return result;
}

namespace {

// One scope panel (dark background, trace, graticule) like the Color page, in 10-bit levels like DaVinci
QImage scopePanel(const Scopes::Data& d, Scopes::Type type, const QString& title, QSize size)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(QColor(18, 18, 20));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect area = img.rect().adjusted(34, 22, -8, -8);
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    p.setPen(QColor(200, 200, 200));
    p.drawText(QRect(6, 3, size.width() - 12, 16), Qt::AlignLeft | Qt::AlignVCenter, title);
    const QColor grid(70, 70, 75);
    if (type == Scopes::Type::Vectorscope) {
        const int side = std::min(area.width(), area.height());
        const QRect sq(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
        p.drawImage(sq, Scopes::trace(d, type));
        p.setPen(grid);
        p.drawEllipse(sq);
        p.drawLine(sq.center().x(), sq.top(), sq.center().x(), sq.bottom());
        p.drawLine(sq.left(), sq.center().y(), sq.right(), sq.center().y());
        // Skin tone line (about 123° like DaVinci's)
        p.setPen(QColor(200, 150, 90, 160));
        const double a = 123.0 * M_PI / 180.0;
        p.drawLine(sq.center(), sq.center() + QPoint(int(std::cos(a) * side / 2), int(-std::sin(a) * side / 2)));
        return img;
    }
    p.setPen(grid);
    for (int i = 0; i <= 4; ++i) {
        const int y = area.bottom() - i * area.height() / 4;
        p.drawLine(area.left(), y, area.right(), y);
        if (type != Scopes::Type::Histogram) // its height is a count, the levels run along x
            p.drawText(QRect(0, y - 7, 30, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(i * 1023 / 4));
    }
    if (type == Scopes::Type::Histogram) {
        // Curves R, G, B, Y over the levels (x), heights relative to the highest bin
        quint32 top = 1;
        for (int c = 0; c < 4; ++c)
            for (int i = 1; i < Scopes::kLevels - 1; ++i) top = std::max(top, d.hist[c][i]);
        const QColor colors[] = {QColor(230, 70, 70), QColor(70, 210, 90), QColor(80, 130, 240), QColor(220, 220, 220)};
        for (int c = 0; c < 4; ++c) {
            QPainterPath path;
            for (int i = 0; i < Scopes::kLevels; ++i) {
                const QPointF pt(area.left() + double(i) / (Scopes::kLevels - 1) * area.width(),
                                 area.bottom() - std::min(1.0, double(d.hist[c][i]) / top) * area.height());
                i == 0 ? path.moveTo(pt) : path.lineTo(pt);
            }
            p.setPen(QPen(colors[c], 1.2));
            p.drawPath(path);
        }
        return img;
    }
    p.drawImage(area, Scopes::trace(d, type));
    return img;
}

// Numbers the AI can judge a frame by: brightness spread, clipping, colour cast, saturation
QJsonObject frameStats(const QImage& frame)
{
    const QImage img = frame.scaledToWidth(std::min(frame.width(), 480)).convertToFormat(QImage::Format_RGB32);
    std::array<int, 256> hist{};
    double sum[3] = {0, 0, 0}, sat = 0;
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x, ++n) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            ++hist[std::clamp(Scopes::luma(r, g, b), 0, 255)];
            sum[0] += r;
            sum[1] += g;
            sum[2] += b;
            double cb = 0, cr = 0;
            Scopes::chroma(r / 255.0, g / 255.0, b / 255.0, &cb, &cr);
            sat += std::hypot(cb, cr);
        }
    }
    if (n == 0) return {};
    const auto percentile = [&](double q) {
        int acc = 0;
        for (int i = 0; i < 256; ++i)
            if ((acc += hist[i]) >= q * n) return i;
        return 255;
    };
    double mean = 0;
    for (int i = 0; i < 256; ++i) mean += double(i) * hist[i];
    const auto pct = [n](double v) { return std::round(v * 1000.0 / n) / 10; };
    int crushed = 0, clipped = 0;
    for (int i = 0; i <= 2; ++i) crushed += hist[i];
    for (int i = 253; i <= 255; ++i) clipped += hist[i];
    const auto lvl = [](double v) { return int(std::lround(v / 255.0 * 1023)); }; // 10-bit like the scopes
    const double avg = (sum[0] + sum[1] + sum[2]) / 3;
    return {{"levels", "10-bit 0..1023 like the scopes"},
            {"luma_mean", lvl(mean / n)}, {"luma_p1", lvl(percentile(0.01))}, {"luma_p50", lvl(percentile(0.5))},
            {"luma_p99", lvl(percentile(0.99))}, {"black_clipped_pct", pct(crushed)}, {"white_clipped_pct", pct(clipped)},
            {"rgb_mean", QJsonArray{lvl(sum[0] / n), lvl(sum[1] / n), lvl(sum[2] / n)}},
            {"cast", avg <= 0 ? QJsonValue("none")
                              : QJsonValue(QJsonObject{{"red", std::round((sum[0] - avg) / avg * 1000) / 10},
                                                       {"green", std::round((sum[1] - avg) / avg * 1000) / 10},
                                                       {"blue", std::round((sum[2] - avg) / avg * 1000) / 10}})},
            {"saturation_mean", std::round(sat / n * 2 * 1000) / 10}}; // 0..100 % of the vectorscope radius
}

} // namespace

QJsonObject cmdScopes(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const bool isProject = isProjectFile(src);
    std::unique_ptr<Session> session;
    MediaSource media;
    ProjectFormat f;
    int length = 0;
    if (isProject) {
        session = std::make_unique<Session>(src);
        session->selectTimeline(a["timeline"]);
        f = session->format();
        length = TimelineOps::endFrame(session->project.timeline());
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        media = openMedia(ma);
        f = media.format;
        length = media.info.length;
        if (!media.info.hasVideo) fail("NO_VIDEO", "the file has no picture");
    }
    if (length <= 0) fail("EMPTY", "nothing to show (empty timeline)");
    const int at = std::clamp(a.contains("at") ? parseTime(a["at"], f, "at") : 0, 0, length - 1);
    const QImage frame = session ? Snapshot::timeline(f, session->project.renderTimeline(), at, 960)
                                 : Snapshot::media(f, media.info.path, at, 960);
    if (frame.isNull()) fail("DECODE_FAILED", QString("could not decode frame %1").arg(at));

    struct Kind {
        const char* id;
        Scopes::Type type;
        const char* title;
    };
    static const Kind kinds[] = {{"waveform", Scopes::Type::Waveform, "Waveform (luma)"},
                                 {"parade", Scopes::Type::Parade, "Parade (R | G | B)"},
                                 {"vectorscope", Scopes::Type::Vectorscope, "Vectorscope"},
                                 {"histogram", Scopes::Type::Histogram, "Histogram (R G B Y)"}};
    QVector<const Kind*> wanted;
    const QString list = a.contains("types") ? a["types"].toString() : "waveform,parade,vectorscope,histogram";
    for (const QString& t : list.split(',', Qt::SkipEmptyParts)) {
        const auto it = std::find_if(std::begin(kinds), std::end(kinds), [&](const Kind& k) { return t.trimmed() == k.id; });
        if (it == std::end(kinds)) fail("BAD_ARGUMENT", "types: waveform, parade, vectorscope, histogram");
        wanted << it;
    }
    const Scopes::Data d = Scopes::compute(frame);
    // One sheet: the frame, then the scopes (2 columns)
    const QSize cell(480, 270);
    QVector<QImage> panels{frame.scaled(cell, Qt::KeepAspectRatio, Qt::SmoothTransformation)};
    for (const Kind* k : wanted) panels << scopePanel(d, k->type, k->title, cell);
    const int cols = 2, rows = (int(panels.size()) + cols - 1) / cols;
    QImage sheet(cols * cell.width(), rows * cell.height(), QImage::Format_RGB32);
    sheet.fill(Qt::black);
    {
        QPainter p(&sheet);
        for (int i = 0; i < panels.size(); ++i) {
            const QPoint pos((i % cols) * cell.width(), (i / cols) * cell.height());
            p.drawImage(pos + QPoint((cell.width() - panels[i].width()) / 2, (cell.height() - panels[i].height()) / 2), panels[i]);
        }
    }
    QJsonObject result{{"source", absolute(src)}, {"frame", at}, {"tc", tc(at, f)}, {"stats", frameStats(frame)}};
    const QString out = a["out"].toString();
    if (!out.isEmpty()) {
        QDir().mkpath(QFileInfo(out).absolutePath());
        if (!sheet.save(out, nullptr, 90)) fail("WRITE_FAILED", "cannot write " + out);
        result["image"] = out;
    }
    if (ctx.mcp) ctx.images = {sheet};
    return result;
}

namespace {

double db1(double v) { return v <= LoudnessMeter::kSilence ? -200.0 : std::round(v * 10) / 10; }

QJsonObject loudnessJson(const AudioAnalysis::Loudness& l)
{
    return {{"integrated_lufs", db1(l.integrated)}, {"range_lu", db1(l.range)},
            {"true_peak_db", db1(l.truePeakDb)}, {"sample_peak_db", db1(l.peakDb)},
            {"max_momentary_lufs", db1(l.maxMomentary)}, {"max_short_term_lufs", db1(l.maxShortTerm)}};
}
} // namespace

QJsonObject cmdLoudness(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const auto progress = [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    };
    QJsonObject result{{"source", absolute(src)}};
    if (!isProjectFile(src)) {
        QJsonObject ma{{"file", src}};
        const MediaSource m = openMedia(ma);
        if (!m.info.hasAudio) fail("NO_AUDIO", "the file has no sound");
        Clip c;
        c.mediaPath = m.info.path;
        c.out = m.info.length - 1;
        c.audioStream = a["audio_stream"].toInt();
        if (c.audioStream >= m.info.audioStreamCount()) fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s)").arg(m.info.audioStreamCount()));
        const auto l = AudioAnalysis::clipLoudness(m.format, c, progress);
        if (!l) fail("DECODE_FAILED", "could not read the sound");
        result["loudness"] = loudnessJson(*l);
        return result;
    }
    Session s(src);
    s.selectTimeline(a["timeline"]);
    const ProjectFormat& f = s.format();
    const QJsonArray clips = a["clips"].toArray();
    if (!clips.isEmpty()) {
        // Every clip on its own (its sound without volume/fades, what 'normalize' measures)
        QJsonArray list;
        for (int i = 0; i < clips.size(); ++i) {
            const Clip* c = TimelineOps::findClip(s.project.timeline(), clips[i].toVariant().toInt());
            if (!c) fail("NOT_FOUND", "no clip with id " + clips[i].toVariant().toString());
            QJsonObject o{{"clip", c->id}};
            if (const auto l = AudioAnalysis::clipLoudness(f, *c)) o["loudness"] = loudnessJson(*l);
            else o["no_sound"] = true;
            list << o;
            progress(double(i + 1) / clips.size());
        }
        result["clips"] = list;
        return result;
    }
    // The mix as exported, optionally only some audio tracks
    Timeline tl = s.project.renderTimeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) fail("EMPTY", "the timeline is empty");
    const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
    const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
    if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
    const QVector<int> only = audioTracksOf(s.project, tl, a["tracks"]);
    if (!only.isEmpty())
        for (int i = 0; i < tl.audio.size(); ++i) {
            tl.audio[i].muted = !only.contains(i);
            tl.audio[i].solo = false;
        }
    const auto l = AudioAnalysis::timelineLoudness(f, tl, from, to, progress);
    if (!l) fail("DECODE_FAILED", "could not mix the sound");
    result["range"] = range(from, to, f);
    result["loudness"] = loudnessJson(*l);
    return result;
}

QJsonObject cmdTranscribe(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    TranscribeRequest req;
    req.model = Extensions::whisperModelPath(a["model"].toString());
    if (req.model.isEmpty())
        fail("NOT_INSTALLED", a.contains("model") ? "whisper model not installed: " + a["model"].toString()
                                                  : "no whisper model installed (see `extensions`, e.g. --install whisper,small)");
    if (Extensions::whisperProgram().isEmpty())
        fail("NOT_INSTALLED", "whisper is not installed (see `extensions`, e.g. --install whisper,small)");
    req.language = a.contains("language") ? a["language"].toString() : "auto";
    req.threads = a["threads"].toInt();
    ProjectFormat f;
    int offset = 0, length = 0;
    if (isProjectFile(src)) {
        // The timeline's sound as it is mixed for the export; times are timeline frames
        Session s(src);
        s.selectTimeline(a["timeline"]);
        f = s.format();
        req.timeline = s.project.renderTimeline();
        req.format = f;
        const int end = TimelineOps::endFrame(req.timeline);
        if (end <= 0) fail("EMPTY", "the timeline is empty");
        req.from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        req.to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (req.to <= req.from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        req.audioTracks = audioTracksOf(s.project, req.timeline, a["tracks"]);
        offset = req.from;
        length = req.to - req.from;
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        const MediaSource m = openMedia(ma);
        if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
        f = m.format;
        req.media = m.info.path;
        req.audioStream = std::clamp(a["audio_stream"].toInt(), 0, std::max(0, m.info.audioStreamCount() - 1));
        length = m.info.length;
    }

    Transcriber t;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&t, &Transcriber::progress, &loop, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
    });
    QObject::connect(&t, &Transcriber::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    QString error;
    if (!t.start(req, &error)) fail("TRANSCRIBE_FAILED", error);
    loop.exec();
    if (!ok) fail("TRANSCRIBE_FAILED", message);

    const Transcript::Result& r = t.result();
    const int maxChars = a.contains("max_chars") ? std::clamp(a["max_chars"].toInt(), 10, 200) : 42;
    QVector<SubtitleCue> cues = Transcript::toCues(r.words, f.rate.fps(), maxChars, 700, offset);
    Transcript::startAtSound(cues, r.levels, f.rate.fps(), offset);
    QJsonArray segments;
    for (const SubtitleCue& c : cues) {
        QJsonObject o = range(c.start, std::min(c.end, offset + length), f);
        o["text"] = c.text;
        segments << o;
    }
    QJsonObject out{{"source", absolute(src)}, {"language", r.language}, {"fps", formatJson(f)["fps"]},
                    {"model", QFileInfo(req.model).fileName()}, {"segments", segments}};
    if (a["words"].toBool()) {
        // Word times for cutting: `to` is where the word is spoken to (pauses after it are left out)
        QJsonArray words;
        const auto fr = [&](qint64 ms) { return offset + int(std::lround(ms / 1000.0 * f.rate.fps())); };
        for (const Transcript::Word& w : r.words)
            words << QJsonObject{{"from", fr(w.from)}, {"to", std::max(fr(w.from) + 1, fr(Transcript::spokenEnd(w)))},
                                 {"text", w.text}};
        out["words"] = words;
    }
    if (a["apply"].toBool()) {
        // Straight onto the timeline as a new subtitle track (live in the app: one undo step there)
        if (!isProjectFile(src)) fail("BAD_ARGUMENT", "apply needs a project as source (\"@app\" or a .schneidi file)");
        QJsonArray list;
        for (const SubtitleCue& c : cues)
            list << QJsonObject{{"from", c.start}, {"to", std::min(c.end, offset + length)}, {"text", c.text}};
        QJsonObject op{{"op", "subtitles"}, {"cues", list}};
        if (a.contains("name")) op["name"] = a["name"];
        QJsonObject edit{{"project", src}, {"ops", QJsonArray{op}}};
        if (a.contains("timeline")) edit["timeline"] = a["timeline"];
        if (list.isEmpty()) fail("NO_SPEECH", "no speech found, nothing to apply");
        const QJsonObject done = execute(*find("edit"), edit, ctx);
        const QJsonArray tracks = done["timeline"]["subtitles"].toArray();
        QJsonObject applied{{"cues", list.size()}, {"saved", done["saved"].toBool()}};
        if (!tracks.isEmpty()) applied["track"] = tracks.last()["track"]; // the new one comes last
        if (done["live"].toBool()) applied["live"] = true;
        out["applied"] = applied;
    }
    if (a.contains("srt")) {
        const QString path = absolute(a["srt"].toString());
        QFile file(path);
        QVector<SubtitleCue> shifted = cues;
        if (!file.open(QIODevice::WriteOnly) || file.write(Subtitles::toSrt(shifted, f.rate.fps(), offset)) < 0)
            fail("WRITE_FAILED", "cannot write " + path);
        out["srt"] = path;
    }
    return out;
}

QJsonObject cmdExtensions(const QJsonObject& a, Context& ctx)
{
    // "whisper,small" -> items; short model names work too
    const auto items = [](const QString& list) {
        QVector<const Extensions::Item*> out;
        for (QString id : list.split(',', Qt::SkipEmptyParts)) {
            id = id.trimmed();
            const Extensions::Item* i = Extensions::find(id);
            if (!i) i = Extensions::find("whisper-model-" + id);
            if (!i) fail("NOT_FOUND", "no extension " + id + " (see `extensions`)");
            out << i;
        }
        return out;
    };
    QJsonArray done;
    for (const Extensions::Item* i : Extensions::withDependencies(items(a["remove"].toString()))) {
        if (!Extensions::remove(*i)) fail("FAILED", "could not remove " + i->id);
        done << QJsonObject{{"removed", i->id}};
    }
    const QVector<const Extensions::Item*> install = Extensions::withDependencies(items(a["install"].toString()));
    qint64 total = 0, before = 0;
    for (const Extensions::Item* i : install) total += i->size;
    for (const Extensions::Item* i : install) {
        if (Extensions::isInstalled(*i) && !a["reinstall"].toBool()) {
            before += i->size;
            continue;
        }
        Extensions::Installer installer;
        QEventLoop loop;
        bool ok = false;
        QString message;
        QObject::connect(&installer, &Extensions::Installer::progress, &loop, [&](qint64 received, qint64) {
            if (ctx.progress && total > 0) ctx.progress(double(before + received) / total);
        });
        QObject::connect(&installer, &Extensions::Installer::finished, &loop, [&](bool success, const QString& msg) {
            ok = success;
            message = msg;
            loop.quit();
        });
        QString error;
        if (!installer.start(*i, &error)) fail("DOWNLOAD_FAILED", error);
        loop.exec();
        if (!ok) fail("DOWNLOAD_FAILED", i->id + ": " + message);
        before += i->size;
        done << QJsonObject{{"installed", i->id}};
    }
    QJsonArray list;
    for (const Extensions::Item& i : Extensions::catalog()) {
        QJsonObject o{{"id", i.id}, {"name", T(i.name)}, {"description", T(i.description)},
                      {"download_mb", std::round(i.size / 1e5) / 10}, {"installed", Extensions::isInstalled(i)}};
        list << o;
    }
    QJsonObject out{{"extensions", list}, {"folder", Extensions::rootDir()}};
    if (!done.isEmpty()) out["done"] = done;
    if (Extensions::find("whisper") == nullptr && !Extensions::whisperProgram().isEmpty())
        out["whisper_program"] = Extensions::whisperProgram(); // own build on PATH
    return out;
}

} // namespace Cli::detail
