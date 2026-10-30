// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: check before export (core/ExportCheck.h) – a project timeline before rendering, or a rendered file
// before handing it over
#include "cli/AnalysisDetail.h"

#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Bundle.h"
#include "engine/CheckMeasure.h"
#include "engine/video/Scan.h"

#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace Cli::detail {

namespace {

double round1(double v) { return std::round(v * 10) / 10; }
QString num1(double v) { return QString::number(v, 'f', 1).replace('.', I18n::decimalPoint()); }

ExportCheck::Options options(const QJsonObject& a)
{
    ExportCheck::Options o;
    if (a.contains("target_lufs")) o.targetLufs = a["target_lufs"].toDouble();
    return o;
}

// Sound of a project range or a file: peaks per frame + loudness, measured with the same engine as `loudness`
struct Measured {
    std::vector<float> peaks;
    AudioAnalysis::Loudness loudness;
};

QJsonObject measuredJson(const Measured& m)
{
    if (m.loudness.integrated <= -200) return {{"sound", false}};
    return {{"integrated_lufs", round1(m.loudness.integrated)}, {"true_peak_db", round1(m.loudness.truePeakDb)},
            {"range_lu", round1(m.loudness.range)}};
}

// Black and frozen picture in a rendered file, in-process on a small copy (engine/video/Scan) with the rules of
// ffmpeg's blackdetect (d=1, pix_th=0.10: 98 % of the pixels at most 10 % above black, for 1 s) and freezedetect
// (n=-60dB, d=3: every plane differs from the picture before by at most 0.1 % on average, for 3 s)
QVector<ExportCheck::Issue> pictureOfFileOwn(const MediaSource& m, Context& ctx, double progressFrom, double progressTo)
{
    const ProjectFormat& f = m.format;
    struct Sample {
        double seconds;
        bool black, still;
    };
    std::vector<Sample> samples;
    std::mutex mutex;
    Video::Scan::Options o;
    o.width = 320;
    o.height = -2;
    o.format = AV_PIX_FMT_YUV420P;
    const Video::Scan::Result r = Video::Scan::run(
        m.info.path, o,
        [&](const Video::Scan::Frame& fr, const Video::Scan::Frame* previous) {
            const int limit = fr.fullRange ? int(0.10 * 255) : int(16 + 0.10 * (235 - 16));
            int64_t dark = 0;
            for (int y = 0; y < fr.height; ++y) {
                const uint8_t* row = fr.data[0] + size_t(y) * size_t(fr.linesize[0]);
                for (int x = 0; x < fr.width; ++x) dark += row[x] <= limit;
            }
            const bool black = dark >= 0.98 * fr.width * fr.height;
            bool still = previous != nullptr;
            for (int p = 0; p < 3 && still; ++p) {
                const int w = p ? (fr.width + 1) / 2 : fr.width, h = p ? (fr.height + 1) / 2 : fr.height;
                uint64_t sad = 0;
                for (int y = 0; y < h; ++y) {
                    const uint8_t* a = fr.data[p] + size_t(y) * size_t(fr.linesize[p]);
                    const uint8_t* b = previous->data[p] + size_t(y) * size_t(previous->linesize[p]);
                    for (int x = 0; x < w; ++x) sad += uint64_t(std::abs(int(a[x]) - int(b[x])));
                }
                still = double(sad) / (double(w) * h * 255.0) <= 0.001; // -60 dB
            }
            std::lock_guard lock(mutex);
            samples.push_back({fr.seconds, black, still});
        },
        [&](double p) {
            if (ctx.progress) ctx.progress(progressFrom + (progressTo - progressFrom) * p);
            return true;
        });
    if (!r.ok) fail("DECODE_FAILED", "could not read the video of " + m.info.path + ": " + r.error);
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.seconds < b.seconds; });

    QVector<ExportCheck::Issue> out;
    const auto frame = [&f](double s) { return int(std::lround(s * f.rate.fps())); };
    const double end = m.info.length / f.rate.fps();
    double blackFrom = -1, stillFrom = -1;
    const auto black = [&](double a, double b) {
        if (b - a < 1.0) return;
        const int a0 = frame(a), b0 = std::min(m.info.length, frame(b));
        out << ExportCheck::Issue{ExportCheck::Severity::Warning, "black_file",
                                  T("%1 s schwarzes Bild").arg(num1(seconds(b0 - a0, f))), a0, b0};
    };
    QVector<ExportCheck::Issue> frozenList;
    const auto frozen = [&](double a, double b, bool toEnd) {
        if (b - a < 3.0) return;
        const int a0 = frame(a), b0 = toEnd ? m.info.length : frame(b);
        frozenList << ExportCheck::Issue{ExportCheck::Severity::Info, "frozen",
                                         T("%1 s Standbild (nichts bewegt sich)").arg(num1(seconds(b0 - a0, f))), a0, b0};
    };
    for (size_t i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        if (s.black && blackFrom < 0) blackFrom = s.seconds;
        if (!s.black && blackFrom >= 0) {
            black(blackFrom, s.seconds);
            blackFrom = -1;
        }
        // A still run starts at the picture the next ones equal
        if (s.still && stillFrom < 0) stillFrom = i > 0 ? samples[i - 1].seconds : s.seconds;
        if (!s.still && stillFrom >= 0) {
            frozen(stillFrom, s.seconds, false);
            stillFrom = -1;
        }
    }
    if (blackFrom >= 0) black(blackFrom, end);
    if (stillFrom >= 0) frozen(stillFrom, end, true);
    // ffmpeg's log order: the black ones as they end, the frozen ones after (both in time order)
    return out + frozenList;
}

// The same with ffmpeg's blackdetect/freezedetect (SCHNEIDI_ANALYSIS=ffmpeg, comparison)
QVector<ExportCheck::Issue> pictureOfFile(const MediaSource& m, Context& ctx, double progressFrom, double progressTo)
{
    if (!ffmpegAnalysis()) return pictureOfFileOwn(m, ctx, progressFrom, progressTo);
    QVector<ExportCheck::Issue> out;
    const ProjectFormat& f = m.format;
    QProcess ff;
    ff.setProgram(Bundle::tool("ffmpeg"));
    ff.setArguments({"-hide_banner", "-nostdin", "-nostats", "-loglevel", "info", "-i", m.info.path, "-map", "0:v:0",
                     "-an", "-sn", "-vf",
                     "setpts=PTS-STARTPTS,scale=320:-2,blackdetect=d=1:pix_th=0.10,freezedetect=n=-60dB:d=3",
                     "-f", "null", "-", "-progress", "pipe:1"});
    ff.start();
    if (!ff.waitForStarted()) fail("TOOL_MISSING", "ffmpeg not found (needed to check the picture)");
    const double total = m.info.length / f.rate.fps();
    QByteArray log;
    while (ff.state() != QProcess::NotRunning) {
        ff.waitForReadyRead(200);
        log += ff.readAllStandardError();
        for (const QByteArray& line : ff.readAllStandardOutput().split('\n'))
            if (line.startsWith("out_time_us=") && ctx.progress && total > 0)
                ctx.progress(progressFrom + (progressTo - progressFrom) * std::clamp(line.mid(12).toDouble() / 1e6 / total, 0.0, 1.0));
    }
    log += ff.readAllStandardError();
    if (ff.exitStatus() != QProcess::NormalExit || ff.exitCode() != 0)
        fail("DECODE_FAILED", "ffmpeg could not read the video: " + QString::fromUtf8(log.right(400)).trimmed());

    const auto frame = [&f](double s) { return int(std::lround(s * f.rate.fps())); };
    static const QRegularExpression blackRe("black_start:([0-9.]+) black_end:([0-9.]+)"),
        freezeStartRe("freeze_start: ([0-9.]+)"), freezeEndRe("freeze_end: ([0-9.]+)");
    int freezeStart = -1;
    const auto frozen = [&](int a, int b) {
        out << ExportCheck::Issue{ExportCheck::Severity::Info, "frozen",
                                  T("%1 s Standbild (nichts bewegt sich)").arg(num1(seconds(b - a, f))), a, b};
    };
    for (const QString& line : QString::fromUtf8(log).split('\n')) {
        if (const auto b = blackRe.match(line); b.hasMatch()) {
            const int a0 = frame(b.captured(1).toDouble()), b0 = std::min(m.info.length, frame(b.captured(2).toDouble()));
            out << ExportCheck::Issue{ExportCheck::Severity::Warning, "black_file",
                                      T("%1 s schwarzes Bild").arg(num1(seconds(b0 - a0, f))), a0, b0};
        } else if (const auto s = freezeStartRe.match(line); s.hasMatch()) {
            freezeStart = frame(s.captured(1).toDouble());
        } else if (const auto e = freezeEndRe.match(line); e.hasMatch() && freezeStart >= 0) {
            frozen(freezeStart, frame(e.captured(1).toDouble()));
            freezeStart = -1;
        }
    }
    if (freezeStart >= 0) frozen(freezeStart, m.info.length); // still frozen at the end
    return out;
}

QJsonObject issueJson(const ExportCheck::Issue& i, const ProjectFormat& f)
{
    QJsonObject o{{"severity", ExportCheck::severityId(i.severity)}, {"check", i.check}, {"message", i.message}};
    if (i.from >= 0) {
        const QJsonObject r = range(i.from, i.to, f);
        for (auto it = r.begin(); it != r.end(); ++it) o[it.key()] = it.value();
    }
    if (i.clip) o["clip"] = i.clip;
    if (!i.clips.isEmpty()) {
        QJsonArray clips;
        for (int id : i.clips) clips << id;
        o["clips"] = clips;
    }
    return o;
}

// The Deliver page's Fix button as ops for `edit`: the model applies them instead of judging the numbers itself
// (a small one called a true peak of -0.1 dBTP "ok for H.264"). Empty when the sound needs no such fix.
QJsonObject fixJson(const ExportCheck::SoundFix& fix, const QJsonValue& timeline)
{
    if (!fix.any()) return {};
    QJsonObject mixer{{"op", "mixer"}, {"track", "master"}};
    QStringList what;
    if (fix.limiter) {
        mixer["limiter"] = true;
        mixer["limiter_db"] = fix.ceiling;
        what << QString("master limiter on at %1 dBTP").arg(fix.ceiling, 0, 'f', 1);
    }
    if (fix.level) {
        mixer["volume_db"] = fix.volumeDb;
        what << QString("master fader %1%2 dB").arg(fix.change > 0 ? "+" : "").arg(fix.change, 0, 'f', 1);
    }
    QJsonArray ops;
    if (!timeline.isUndefined() && !timeline.isNull()) ops << QJsonObject{{"op", "timeline"}, {"timeline", timeline}};
    ops << mixer;
    return {{"what", what.join(", ")}, {"ops", ops}, {"how", "apply with `edit` (one undo step), then `check` again"}};
}

} // namespace

QJsonObject cmdCheck(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const ExportCheck::Options opt = options(a);
    const bool withSound = !a.contains("sound") || a["sound"].toBool();
    const auto progress = [&ctx](double from, double to) {
        return [&ctx, from, to](double p) {
            if (ctx.progress) ctx.progress(from + (to - from) * p);
            return true;
        };
    };
    QVector<ExportCheck::Issue> issues;
    Measured measured;
    QJsonObject result{{"source", absolute(src)}};
    QJsonObject fix;
    ProjectFormat f;

    if (isProjectFile(src)) {
        Session s(src);
        s.selectTimeline(a["timeline"]);
        f = s.format();
        const Timeline& tl = s.project.timeline();
        const int end = TimelineOps::endFrame(tl);
        if (end <= 0) s.failEmpty();
        const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        result["range"] = range(from, to, f);
        issues += ExportCheck::timeline(tl, from, to, f, opt);
        issues += ExportCheck::layout(tl, from, to, f, opt);
        // The timeline as exported (render timeline: compound clips, limiter, mute/solo)
        const Timeline render = s.project.renderTimeline();
        const auto media = [&s](const QString& path) { return s.project.mediaInfo(path); };
        const double scanned = withSound ? 0.1 : 0.8;
        if (const auto st = CheckMeasure::stutter(
                f, tl, from, to, [&](const QString& p) { return media(p) ? media(p)->length : 0; }, progress(0, scanned)))
            issues += *st;
        issues += CheckMeasure::hook(f, render, from, to, opt);
        if (withSound) {
            std::vector<float> peaks;
            const auto loud = AudioAnalysis::timelineLoudness(f, render, from, to, progress(scanned, 0.55), &peaks);
            if (!loud) fail("DECODE_FAILED", "could not mix the sound");
            measured = {peaks, *loud};
            const QVector<ExportCheck::Issue> heard =
                ExportCheck::sound(measured.peaks, from, loud->integrated, loud->truePeakDb, f, opt, render.masterLimiter);
            issues += heard;
            fix = fixJson(ExportCheck::soundFix(heard, loud->integrated, tl, opt), a["timeline"]);
            const auto picture = [&](const QString& p) { return media(p) && media(p)->hasVideo && !media(p)->isImage; };
            if (const auto balance = CheckMeasure::mix(f, render, from, to, picture, opt, progress(0.55, 1)))
                issues += *balance;
        }
    } else {
        const MediaSource m = openMedia(QJsonObject{{"file", src}});
        f = m.format;
        result["frames"] = m.info.length;
        if (m.info.hasVideo && !m.info.isImage) {
            issues += pictureOfFile(m, ctx, 0, withSound && m.info.hasAudio ? 0.5 : 1);
        } else {
            issues << ExportCheck::Issue{ExportCheck::Severity::Info, "no_video", T("Die Datei hat kein Bild (nur Ton)")};
        }
        if (!m.info.hasAudio) {
            issues << ExportCheck::Issue{ExportCheck::Severity::Warning, "no_audio", T("Die Datei hat keinen Ton")};
        } else if (withSound) {
            Clip c;
            c.mediaPath = m.info.path;
            c.out = m.info.length - 1;
            const double start = m.info.hasVideo ? 0.5 : 0;
            const auto peaks = AudioAnalysis::clipFramePeaks(f, c, progress(start, start + (1 - start) / 2));
            const auto loud = AudioAnalysis::clipLoudness(f, c, progress(start + (1 - start) / 2, 1));
            if (!peaks || !loud) fail("DECODE_FAILED", "could not read the sound of " + m.info.path);
            measured = {*peaks, *loud};
            issues += ExportCheck::sound(measured.peaks, 0, loud->integrated, loud->truePeakDb, f, opt);
        }
    }

    // Errors first, then by time
    std::stable_sort(issues.begin(), issues.end(), [](const ExportCheck::Issue& x, const ExportCheck::Issue& y) {
        if (x.severity != y.severity) return int(x.severity) < int(y.severity);
        return x.from < y.from;
    });
    QJsonArray list;
    for (const ExportCheck::Issue& i : issues) list << issueJson(i, f);
    result["issues"] = list;
    result["errors"] = ExportCheck::errors(issues);
    result["warnings"] = ExportCheck::warnings(issues);
    if (withSound) {
        QJsonObject m = measuredJson(measured);
        m["target_lufs"] = opt.targetLufs;
        result["sound"] = m;
    }
    if (!fix.isEmpty()) result["fix"] = fix;
    return result;
}

} // namespace Cli::detail
