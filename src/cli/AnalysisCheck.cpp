// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: check before export (core/ExportCheck.h) – a project timeline before rendering, or a rendered file
// before handing it over
#include "cli/AnalysisDetail.h"

#include "core/CodeTrust.h"
#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"
#include "core/TimelineStory.h"
#include "core/TranscriptCut.h"
#include "engine/AudioAnalysis.h"
#include "engine/Bundle.h"
#include "engine/CheckMeasure.h"
#include "engine/HardwareEncoders.h"
#include "engine/picture/CustomEffects.h"
#include "engine/Snapshot.h"
#include "engine/video/Scan.h"

#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cstring>
#include <functional>
#include <cmath>
#include <mutex>

namespace Cli::detail {

namespace {

double round1(double v) { return std::round(v * 10) / 10; }
QString num1(double v) { return QString::number(v, 'f', 1).replace('.', I18n::decimalPoint()); }

ExportCheck::Options options(const QJsonObject& a)
{
    ExportCheck::Options o;
    // "for", not "platform": QGuiApplication takes --platform from the command line
    if (a.contains("for")) {
        const ShortPlatform* p = shortPlatform(a["for"].toString());
        if (!p) {
            QStringList ids;
            for (const ShortPlatform& sp : shortPlatforms()) ids << sp.alias;
            fail("BAD_ARGUMENT", "for must be " + ids.join(", ") + " (the platform)");
        }
        o.maxLengthS = p->maxSeconds;
        o.platform = QString::fromLatin1(p->name);
        o.targetLufs = p->targetLufs;
        o.maxTruePeakDb = p->maxTruePeakDb;
    }
    if (a.contains("target_lufs")) o.targetLufs = a["target_lufs"].toDouble();
    if (a.contains("max_still_s")) {
        o.maxStillS = a["max_still_s"].toDouble();
        if (!(o.maxStillS >= 0.5 && o.maxStillS <= 600)) fail("BAD_ARGUMENT", "max_still_s must be between 0.5 and 600");
    }
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
    if (i.at >= 0) {
        o["at"] = i.at;
        o["at_s"] = seconds(i.at, f);
    }
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

constexpr int kMaxListed = 50;   // still stretches in the answer
constexpr int kMaxSuggested = 60; // break points in `suggest`

QJsonValue round2(double v) { return std::round(v * 100) / 100; }

// Pace of the range: changes per second, the longest stretch without one, the curve, per story section
QJsonObject paceJson(const ExportCheck::Pace& p, const ProjectFormat& f)
{
    QJsonObject kinds;
    for (const ExportCheck::Change& c : p.changes) {
        const QString k = QString::fromLatin1(ExportCheck::changeKindId(c.kind));
        kinds[k] = kinds[k].toInt() + 1;
    }
    // At most kMaxListed still stretches listed (a long static podcast has hundreds), still_count says how many
    QJsonArray still, curve, sections;
    for (const ExportCheck::Stretch& s : p.still)
        if (still.size() < kMaxListed) still << range(s.from, s.to, f);
    for (double v : p.curve) curve << v;
    for (const ExportCheck::SectionPace& sp : p.sections) {
        QJsonObject o{{"kind", TimelineStory::kindName(sp.section.kind)},
                      {"from_s", seconds(sp.section.from, f)},
                      {"to_s", seconds(sp.section.to, f)},
                      {"changes_per_s", round2(sp.changesPerS)},
                      {"longest_s", seconds(sp.longest.length(), f)},
                      {"longest_at_s", seconds(sp.longest.from, f)}};
        if (!sp.section.name.isEmpty()) o["name"] = sp.section.name;
        sections << o;
    }
    QJsonObject o{{"changes", int(p.changes.size())},
                  {"kinds", kinds},
                  {"changes_per_s", round2(p.changesPerS)},
                  {"longest_s", seconds(p.longest.length(), f)},
                  {"at", p.longest.from},
                  {"at_s", seconds(p.longest.from, f)},
                  {"at_tc", tc(p.longest.from, f)},
                  {"still", still},
                  {"still_count", int(p.still.size())},
                  {"curve_step_s", p.curveStepS},
                  {"curve", curve}};
    if (!sections.isEmpty()) o["sections"] = sections;
    return o;
}

// Ways to break the still stretches, as ops for `edit` with a reason each: a mild punch-in (accent) where a picture
// is, and as an alternative the words spoken there as a text. Creative suggestions, never applied by `check` or
// `short` itself (unlike `fix`). Empty when nothing is too still.
QJsonObject suggestJson(const ExportCheck::Pace& p, const Timeline& tl, const QVector<TranscriptCut::Word>& words,
                        const ProjectFormat& f, const ExportCheck::Options& o, const QJsonValue& timeline)
{
    QJsonArray ops, texts;
    int points = 0;
    bool limited = false;
    for (const ExportCheck::Stretch& s : p.still) {
        QVector<int> at = ExportCheck::breakPoints(tl, s, f, o);
        if (points + at.size() > kMaxSuggested) {
            at.resize(std::max(0, kMaxSuggested - points));
            limited = true;
        }
        if (at.isEmpty()) continue;
        const QString why = QString("pace_slow: %1 s without a cut, zoom or text from %2")
                                .arg(seconds(s.length(), f))
                                .arg(tc(s.from, f));
        QJsonArray times;
        for (int x : at) times << x;
        ops << QJsonObject{{"op", "accent"}, {"at", times}, {"preset", "punch"}, {"strength", 0.5},
                           {"why", why + " - a mild punch-in"}};
        points += at.size();
        const int span = int(std::lround(1.5 * f.rate.fps()));
        for (int x : at) {
            QStringList said;
            for (const TranscriptCut::Word& w : words)
                if (w.from >= x && w.from < x + span && said.size() < 5) said << w.text.trimmed();
            if (said.isEmpty()) continue;
            texts << QJsonObject{{"op", "title"}, {"text", said.join(' ')}, {"at", x}, {"duration", span},
                                 {"why", why + " - the words spoken there as a text"}};
        }
    }
    if (ops.isEmpty()) return {};
    const bool named = !timeline.isUndefined() && !timeline.isNull();
    if (named) ops.prepend(QJsonObject{{"op", "timeline"}, {"timeline", timeline}});
    QJsonObject out{{"what", QString("mild punch-in (accent punch, strength 0.5) at %1 points in %2 still stretches")
                                 .arg(points)
                                 .arg(ops.size() - (named ? 1 : 0))},
                    {"ops", ops},
                    {"how", "a creative suggestion, not applied by `check` or `short`: apply with `edit` (one undo "
                            "step) if it suits the video - or cut, zoom or add text yourself - then `check` again"}};
    if (limited)
        out["limited"] = QString("only the first %1 points: the video is still for long stretches - more cuts, "
                                 "b-roll or a calmer pace on purpose?")
                             .arg(kMaxSuggested);
    if (!texts.isEmpty()) {
        if (named) texts.prepend(QJsonObject{{"op", "timeline"}, {"timeline", timeline}});
        out["text_ops"] = texts;
    }
    return out;
}

QString readText(const QString& path)
{
    QFile file(path);
    if (!file.exists()) fail("NOT_FOUND", "retention file not found: " + path);
    if (!file.open(QIODevice::ReadOnly)) fail("BAD_ARGUMENT", "cannot read the retention file: " + path);
    if (file.size() > 8 * 1024 * 1024) fail("BAD_ARGUMENT", "the retention file is larger than 8 MB");
    return QString::fromUtf8(file.readAll());
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
    QJsonObject fix, suggest;
    QVector<ExportCheck::Change> changes; // pace changes of a project's range (for the retention drops)
    double length = 0;                    // seconds checked
    int retentionFrom = 0;
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
        issues += CheckMeasure::bars(f, tl, from, to);
        // Custom effects this machine cannot draw (shader without a graphics card or qsb, filter chain without
        // libavfilter): rendered without them
        for (const Track& t : tl.video)
            for (const Clip& c : t.clips) {
                if (t.hidden || !c.enabled || c.end() <= from || c.start >= to) continue;
                for (const EffectInstance& e : c.effects) {
                    QString why;
                    if (!e.enabled || !Picture::Custom::isCustom(e.effectId) || Picture::Custom::canRender(e.effectId, &why))
                        continue;
                    issues << ExportCheck::Issue{ExportCheck::Severity::Warning, "custom_effect_skipped",
                                                 T("Effekt „%1“ wird hier übersprungen: %2").arg(e.effectId, why),
                                                 std::max(from, c.start), std::min(to, c.end()), c.id};
                }
            }
        // Custom code the user did not confirm (a project or recipe from someone else, core/CodeTrust.h): kept but not
        // run until trusted (render/edit/open with trust_custom_code:true, or "Ausführen" in the app). Only what the render uses: enabled clips on shown tracks of the open timeline and the sequences its
        // compound/multicam clips show (not hidden tracks or sequences nothing uses)
        {
            const ProjectData data = s.project.data();
            QVector<std::pair<CodeTrust::Item, const Clip*>> found; // the item and the open timeline's clip showing it
            QSet<int> seen{s.project.currentSequence()};
            std::function<void(const Timeline&, const Clip*)> scan = [&](const Timeline& t, const Clip* top) {
                for (const Track& tr : t.video) {
                    if (tr.hidden) continue;
                    for (const Clip& c : tr.clips) {
                        if (!c.enabled) continue;
                        const Clip* shown = top ? top : &c;
                        if (shown->end() <= from || shown->start >= to) continue;
                        for (const EffectInstance& e : c.effects)
                            for (CodeTrust::Item it : CodeTrust::itemsOf(e))
                                if (!CodeTrust::isTrusted(it.code)) {
                                    it.clip = c.id;
                                    found << std::pair{it, shown};
                                }
                        if (c.isNested() && !seen.contains(c.sequenceId)) {
                            seen << c.sequenceId;
                            for (const Sequence& q : data.sequences)
                                if (q.id == c.sequenceId) scan(q.timeline, shown);
                        }
                    }
                }
            };
            scan(tl, nullptr);
            for (const auto& [it, c] : found) {
                const QString what = it.node ? QStringLiteral("comp:%1 (node %2)").arg(it.effect).arg(it.node) : it.effect;
                QString first = it.code.trimmed().section(QLatin1Char('\n'), 0, 0).left(60);
                issues << ExportCheck::Issue{ExportCheck::Severity::Warning, "custom_code_untrusted",
                                             T("Eigener Code in „%1“ läuft erst, wenn du ihm vertraust (nicht von dir "
                                               "geschrieben): „%2“ – trust_custom_code:true oder „Ausführen“ in der App")
                                                 .arg(what, first),
                                             c ? std::max(from, c->start) : -1, c ? std::min(to, c->end()) : -1, it.clip};
            }
        }
        // Keys of compositing graphs: the matte at the middle of the clip, small (one frame per key node, at most 8)
        int keys = 0;
        for (const Track& t : tl.video)
            for (const Clip& c : t.clips) {
                CompGraph g;
                const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
                if (t.hidden || !c.enabled || c.end() <= from || c.start >= to || !e || !::Comp::graphOf(*e, &g)) continue;
                const QVector<int> used = g.evalOrder();
                for (const CompNode& n : g.nodes) {
                    if (n.type != QLatin1String("key") || !used.contains(n.id) || ++keys > 8) continue;
                    int start = 0, stop = 0;
                    const Timeline only = compNodeTimeline(s, {{"clip", c.id}, {"node", n.id}, {"view", "matte"}}, &start, &stop);
                    const int mid = std::clamp((std::max(from, start) + std::min(to, stop)) / 2, start, stop - 1);
                    const QImage m = Snapshot::Source::timeline(f, only)->frame(mid, 320).convertToFormat(QImage::Format_Grayscale8);
                    if (m.isNull()) continue;
                    std::vector<uint8_t> alpha(size_t(m.width()) * m.height());
                    for (int y = 0; y < m.height(); ++y)
                        std::memcpy(alpha.data() + size_t(y) * m.width(), m.constScanLine(y), size_t(m.width()));
                    if (auto i = ExportCheck::keyEdges(alpha.data(), m.width(), m.height(), c.id, std::max(from, c.start),
                                                       std::min(to, c.end())))
                        issues << *i;
                }
            }
        // H.264 export without a graphics card encoder and without x264: the free encoder needs twice the bit rate
        if (HardwareEncoders::needsBetterEncoder("h264"))
            issues << ExportCheck::Issue{ExportCheck::Severity::Info, "encoder_quality",
                                         HardwareEncoders::betterQualityHint(HardwareEncoders::softwareCodec("h264"))};
        if (withSound) {
            std::vector<float> peaks;
            const int soundTo = ExportCheck::soundEnd(from, to, f, &issues);
            const auto loud = AudioAnalysis::timelineLoudness(f, render, from, soundTo, progress(scanned, 0.55), &peaks);
            if (!loud) fail("DECODE_FAILED", "could not mix the sound");
            measured = {peaks, *loud};
            const QVector<ExportCheck::Issue> heard =
                ExportCheck::sound(measured.peaks, from, loud->integrated, loud->truePeakDb, f, opt, render.masterLimiter);
            issues += heard;
            fix = fixJson(ExportCheck::soundFix(heard, loud->integrated, tl, opt), a["timeline"]);
            const auto picture = [&](const QString& p) { return media(p) && media(p)->hasVideo && !media(p)->isImage; };
            if (const auto balance = CheckMeasure::mix(f, render, from, soundTo, picture, opt, progress(0.55, 1)))
                issues += *balance;
        }
        // Pace and story: from the timeline, the measured peaks and the transcripts (no decoding of their own)
        const ExportCheck::Pace pace = ExportCheck::pace(tl, from, to, f, opt);
        issues += ExportCheck::paceIssues(pace, from, to, f, opt);
        issues += ExportCheck::jumpCuts(tl, from, to, f);
        changes = pace.changes;
        const QVector<TranscriptCut::Word> words = TranscriptCut::timelineWords(tl, s.project.media(), f.rate.fps());
        QVector<int> spoken;
        for (const TranscriptCut::Word& w : words) spoken << w.from;
        QString strongBy;
        const QVector<int> strong =
            ExportCheck::strongMoments(tl, from, to, f, withSound ? &measured.peaks : nullptr, spoken, &strongBy);
        issues += ExportCheck::story(tl, from, to, f, strong, withSound || !words.isEmpty(), spoken, opt);
        result["pace"] = paceJson(pace, f);
        QJsonObject story{{"sections", int(tl.sections.size())}};
        if (!strong.isEmpty()) {
            story["first_strong_s"] = seconds(strong.first() - from, f);
            story["first_strong_by"] = strongBy;
        }
        if (tl.sections.isEmpty())
            story["note"] = "no story sections: only the strong moments are checked (story_hook_late); sections "
                            "(hook, build, question, turn, resolution, cta) add story_no_hook, story_open_question, "
                            "story_resolution_late, story_no_cta, story_gap";
        result["story"] = story;
        suggest = suggestJson(pace, tl, words, f, opt, a["timeline"]);
        length = seconds(to - from, f);
        retentionFrom = from;
    } else {
        const MediaSource m = openMedia(QJsonObject{{"file", src}});
        f = m.format;
        result["frames"] = m.info.length;
        length = m.info.length / f.rate.fps();
        issues += ExportCheck::length(0, m.info.length, f, opt);
        if (m.info.isImage) {
            // A still has neither motion nor sound to check: say what it is instead of "no picture" and "no sound"
            issues << ExportCheck::Issue{ExportCheck::Severity::Info, "still_image",
                                         T("Die Datei ist ein Standbild: kein Ton, nichts zu prüfen")};
        } else if (m.info.hasVideo) {
            issues += pictureOfFile(m, ctx, 0, withSound && m.info.hasAudio ? 0.5 : 1);
        } else {
            issues << ExportCheck::Issue{ExportCheck::Severity::Info, "no_video", T("Die Datei hat kein Bild (nur Ton)")};
        }
        if (!m.info.hasAudio) {
            if (!m.info.isImage)
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

    // Retention curve of the published video: where viewers leave, with the change before it
    if (a.contains("retention")) {
        QString error;
        const QVector<ExportCheck::RetentionPoint> curve =
            ExportCheck::retentionFromCsv(readText(absolute(a["retention"].toString())), length, &error);
        if (curve.isEmpty()) fail("BAD_ARGUMENT", "retention: " + error);
        const QVector<ExportCheck::Issue> drops = ExportCheck::retention(curve, changes, retentionFrom, f);
        issues += drops;
        result["retention"] = QJsonObject{{"points", int(curve.size())},
                                          {"start_pct", round2(curve.first().percent)},
                                          {"end_pct", round2(curve.last().percent)},
                                          {"end_s", round2(curve.last().seconds)},
                                          {"drops", int(drops.size())}};
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
    if (!suggest.isEmpty()) result["suggest"] = suggest;
    return result;
}

} // namespace Cli::detail
