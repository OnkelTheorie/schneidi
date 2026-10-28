// schneidi-cli: `overview` – a first look at material in one call. Per stretch of time: how loud and how often
// silent, how much moves, how many cuts and how bright (media file), or which clips play there (project). Cheap in
// tokens: an AI (also a small local model) reads a few lines and then picks the detailed analysis it needs.
#include "cli/AnalysisDetail.h"

#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Bundle.h"

#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Cli::detail {

namespace {

constexpr double kSilentDb = -45; // per-frame peak below = silent
constexpr double kCut = 0.3;      // scene score above = a cut (like `scenes`)

// Window length: given, or about 20 windows on a round number of seconds
int windowFrames(const QJsonObject& a, int length, const ProjectFormat& f)
{
    if (a.contains("window")) {
        const int w = parseTime(a["window"], f, "window");
        if (w <= 0) fail("BAD_ARGUMENT", "'window' must be > 0");
        return w;
    }
    const double seconds = length / f.rate.fps() / 20;
    double pick = 1;
    for (double s : {1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0})
        if (s <= seconds) pick = s;
    return std::max(1, int(std::lround(pick * f.rate.fps())));
}

QString minutes(double s)
{
    const int t = int(std::floor(s));
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

double db(float v) { return 20 * std::log10(std::max(1e-6f, v)); }

// End of the window starting at `from`; a rest shorter than half a window joins the last one
int windowEnd(int from, int w, int length) { return length - (from + w) < w / 2 ? length : from + w; }

// Sound of one window: median and loudest per-frame peak, share of silent frames
struct Sound {
    double medianDb = -120, peakDb = -120;
    int silentPercent = 100;
};
Sound soundOf(const std::vector<float>& peaks, int from, int to)
{
    Sound s;
    from = std::clamp(from, 0, int(peaks.size()));
    to = std::clamp(to, from, int(peaks.size()));
    if (to <= from) return s;
    std::vector<double> d;
    int silent = 0;
    for (int i = from; i < to; ++i) {
        d.push_back(db(peaks[size_t(i)]));
        silent += d.back() < kSilentDb;
    }
    std::vector<double> sorted = d;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    s.medianDb = std::round(sorted[sorted.size() / 2]);
    s.peakDb = std::round(*std::max_element(d.begin(), d.end()));
    s.silentPercent = int(std::lround(100.0 * silent / (to - from)));
    return s;
}

QString soundText(const Sound& s)
{
    if (s.silentPercent >= 95) return "silent";
    QString t = QString("sound %1 dB (peak %2)").arg(s.medianDb).arg(s.peakDb);
    if (s.silentPercent >= 5) t += QString(", %1 % silent").arg(s.silentPercent);
    return t;
}

// Per frame of a video file: scene score (difference to the frame before, 0..1) and brightness (0..255)
struct Picture {
    std::vector<double> score, luma;
};
Picture pictureOf(const MediaSource& m, Context& ctx, double progressFrom, double progressTo)
{
    Picture out;
    out.score.assign(size_t(m.info.length), 0);
    out.luma.assign(size_t(m.info.length), -1);
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     // brightness: showinfo's mean of the Y plane in 8 bit (signalstats' YAVG gave the same, but it
                     // is a GPL-only filter, missing from the GPL-free FFmpeg of the bundled packages)
                     "setpts=PTS-STARTPTS,scale=160:-2,select='gte(scene\\,0)',metadata=mode=print:key=lavfi.scene_score,"
                     "format=yuv420p,showinfo",
                     "-f", "null", "-", "-progress", "pipe:1"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed to look at the picture)");
    const double fps = m.format.rate.fps(), total = m.info.length / fps;
    static const QRegularExpression timeRe("pts_time:([0-9.]+)"), lumaRe(" mean:\\[([0-9.]+)"),
        scoreRe("scene_score=([0-9.]+)");
    int frame = 0;
    QByteArray pending;
    const auto parse = [&](const QByteArray& chunk) {
        pending += chunk;
        const qsizetype end = pending.lastIndexOf('\n');
        if (end < 0) return;
        for (const QByteArray& raw : pending.left(end).split('\n')) {
            const QString line = QString::fromUtf8(raw);
            // showinfo: time and brightness on one line; metadata print: time line, then the score line
            if (const auto t = timeRe.match(line); t.hasMatch())
                frame = std::clamp(int(std::lround(t.captured(1).toDouble() * fps)), 0, std::max(0, m.info.length - 1));
            if (const auto l = lumaRe.match(line); l.hasMatch() && frame < m.info.length)
                out.luma[size_t(frame)] = l.captured(1).toDouble();
            else if (const auto s = scoreRe.match(line); s.hasMatch() && frame < m.info.length)
                out.score[size_t(frame)] = s.captured(1).toDouble();
        }
        pending.remove(0, end + 1);
    };
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        parse(ff.readAllStandardError());
        for (const QByteArray& line : ff.readAllStandardOutput().split('\n'))
            if (line.startsWith("out_time_us=") && ctx.progress && total > 0)
                ctx.progress(progressFrom
                             + (progressTo - progressFrom) * std::clamp(line.mid(12).toDouble() / 1e6 / total, 0.0, 1.0));
    }
    parse(ff.readAllStandardError() + '\n');
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video of " + m.info.path);
    return out;
}

QJsonObject overviewFile(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    const ProjectFormat& f = m.format;
    const int length = m.info.length;
    const int w = windowFrames(a, length, f);
    const bool picture = m.info.hasVideo && !m.info.isImage;
    std::vector<float> peaks;
    if (m.info.hasAudio) {
        Clip c;
        c.mediaPath = m.info.path;
        c.out = length - 1;
        c.audioStream = a["audio_stream"].toInt();
        const double end = picture ? 0.3 : 1;
        if (const auto p = AudioAnalysis::clipFramePeaks(f, c, [&ctx, end](double p) {
                if (ctx.progress) ctx.progress(p * end);
                return true;
            }))
            peaks = *p;
    }
    const Picture pic = picture ? pictureOf(m, ctx, m.info.hasAudio ? 0.3 : 0, 1) : Picture{};
    const int minGap = std::max(1, int(std::lround(0.5 * f.rate.fps())));

    QJsonArray windows;
    QStringList lines;
    int lastCut = -minGap;
    for (int from = 0, to = 0; from < length; from = to) {
        to = windowEnd(from, w, length);
        QJsonObject o = range(from, to, f);
        QStringList text{QString("%1-%2").arg(minutes(from / f.rate.fps()), minutes(to / f.rate.fps()))};
        if (m.info.hasAudio) {
            const Sound s = soundOf(peaks, from, to);
            o["sound_db"] = s.medianDb;
            o["peak_db"] = s.peakDb;
            o["silent_percent"] = s.silentPercent;
            text << soundText(s);
        }
        if (picture) {
            int cuts = 0, n = 0;
            double motion = 0, luma = 0;
            int lumaN = 0;
            for (int i = from; i < to; ++i) {
                const double sc = pic.score[size_t(i)];
                if (sc > kCut && i > 0) {
                    if (i - lastCut >= minGap) ++cuts;
                    lastCut = i;
                } else if (i > 0) {
                    motion += sc;
                    ++n;
                }
                if (pic.luma[size_t(i)] >= 0) {
                    luma += pic.luma[size_t(i)];
                    ++lumaN;
                }
            }
            // Average change between neighbouring frames without the cuts, as 0..100 (still picture 0, a fast
            // game or a shaky handheld 10-30)
            const int moving = n ? int(std::lround(motion / n * 1000)) : 0;
            const int bright = lumaN ? int(std::lround(luma / lumaN / 2.55)) : 0;
            o["motion"] = std::min(100, moving);
            o["cuts"] = cuts;
            o["brightness"] = bright;
            text << QString("motion %1").arg(std::min(100, moving));
            if (cuts) text << (cuts == 1 ? QString("1 cut") : QString("%1 cuts").arg(cuts));
            text << (bright < 8 ? QString("black") : QString("brightness %1 %").arg(bright));
        }
        windows << o;
        lines << text.join("  ");
    }
    return {{"file", m.info.path},
            {"seconds", seconds(length, f)},
            {"window_s", seconds(w, f)},
            {"windows", windows},
            {"text", lines.join('\n')}};
}

QJsonObject overviewProject(const QJsonObject& a, Context& ctx)
{
    const QString src = a["file"].toString();
    Session s(src);
    s.selectTimeline(a["timeline"]);
    const ProjectFormat& f = s.format();
    const Timeline& tl = s.project.timeline();
    const int length = TimelineOps::endFrame(tl);
    if (length <= 0) s.failEmpty();
    const int w = windowFrames(a, length, f);
    const auto peaks = AudioAnalysis::timelineFramePeaks(f, s.project.renderTimeline(), 0, length, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    });
    QJsonArray windows;
    QStringList lines;
    for (int from = 0, to = 0; from < length; from = to) {
        to = windowEnd(from, w, length);
        QJsonObject o = range(from, to, f);
        QStringList text{QString("%1-%2").arg(minutes(from / f.rate.fps()), minutes(to / f.rate.fps()))};
        // What is seen: clips of the visible video tracks there, top track first (titles by their text)
        QStringList seen;
        QJsonArray clips;
        for (int t = tl.video.size() - 1; t >= 0; --t) {
            if (tl.video[t].hidden) continue;
            for (const Clip& c : tl.video[t].clips)
                if (c.enabled && c.end() > from && c.start < to) {
                    clips << c.id;
                    const QString name = c.displayName();
                    if (!seen.contains(name) && seen.size() < 4) seen << name;
                }
        }
        o["clips"] = clips;
        text << (seen.isEmpty() ? QString("no picture") : seen.join(", "));
        if (peaks) {
            const Sound snd = soundOf(*peaks, from, to);
            o["sound_db"] = snd.medianDb;
            o["peak_db"] = snd.peakDb;
            o["silent_percent"] = snd.silentPercent;
            text << soundText(snd);
        }
        windows << o;
        lines << text.join("  ");
    }
    return {{"project", s.path.isEmpty() ? absolute(src) : s.path},
            {"seconds", seconds(length, f)},
            {"window_s", seconds(w, f)},
            {"windows", windows},
            {"text", lines.join('\n')}};
}

} // namespace

QJsonObject cmdOverview(const QJsonObject& a, Context& ctx)
{
    if (isProjectFile(a["file"].toString())) return overviewProject(a, ctx);
    return perFile(a, ctx, overviewFile, 2);
}

} // namespace Cli::detail
