// schneidi-cli: check before export (core/ExportCheck.h) – a project timeline before rendering, or a rendered file
// before handing it over
#include "cli/AnalysisDetail.h"

#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Bundle.h"

#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

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

// Black and frozen picture in a rendered file (ffmpeg blackdetect/freezedetect on a small copy)
QVector<ExportCheck::Issue> pictureOfFile(const MediaSource& m, Context& ctx, double progressFrom, double progressTo)
{
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
    return o;
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
    ProjectFormat f;

    if (isProjectFile(src)) {
        Session s(src);
        s.selectTimeline(a["timeline"]);
        f = s.format();
        const Timeline& tl = s.project.timeline();
        const int end = TimelineOps::endFrame(tl);
        if (end <= 0) fail("EMPTY", "the timeline is empty");
        const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        result["range"] = range(from, to, f);
        issues += ExportCheck::timeline(tl, from, to, f, opt);
        if (withSound) {
            // The mix as exported (render timeline: compound clips, limiter, mute/solo)
            const Timeline mix = s.project.renderTimeline();
            const auto peaks = AudioAnalysis::timelineFramePeaks(f, mix, from, to, progress(0, 0.5));
            const auto loud = AudioAnalysis::timelineLoudness(f, mix, from, to, progress(0.5, 1));
            if (!peaks || !loud) fail("DECODE_FAILED", "could not mix the sound");
            measured = {*peaks, *loud};
            issues += ExportCheck::sound(measured.peaks, from, loud->integrated, loud->truePeakDb, f, opt, mix.masterLimiter);
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
    return result;
}

} // namespace Cli::detail
