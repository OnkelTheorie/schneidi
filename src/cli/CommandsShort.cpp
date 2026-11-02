// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `short`: from recordings (or a timeline) to a finished vertical short in one call. Only strings the
// other commands together (probe, new, highlights, edit keep/new_timeline, transcribe, check + its fix, render), each
// through execute() like a caller would: live in the app with "@app", no logic of its own beyond picking the moments
// (core/ShortPick).
#include "cli/CommandsDetail.h"
#include "cli/Live.h"

#include "core/I18n.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/ShortPick.h"
#include "engine/Extensions.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

// Kill feed settings per game as the highlights guide documents them
struct GameHint {
    const char* id;
    const char* region;
    const char* color;
    int tolerance; // 0 = default
};
constexpr GameHint kGames[] = {
    {"cs2", "0.72,0.04,0.28,0.2", "#d8444a", 40},
    {"valorant", "0.70,0.04,0.22,0.23", "#e7e371", 0},
};

const GameHint* gameHint(const QString& id)
{
    for (const GameHint& g : kGames)
        if (id.compare(QLatin1String(g.id), Qt::CaseInsensitive) == 0) return &g;
    return nullptr;
}

// The game from the folder (a clip tool's "cs2/") or the file name ("cs2_clip_…", "valorant-…")
const GameHint* guessGame(const QString& file)
{
    const QFileInfo fi(file);
    const QString dir = fi.absoluteDir().dirName().toLower(), name = fi.fileName().toLower();
    for (const GameHint& g : kGames) {
        const QString id = QLatin1String(g.id);
        if (dir == id || name.startsWith(id + "_") || name.startsWith(id + "-") || name.startsWith(id + " ")) return &g;
    }
    return nullptr;
}

// "<base>-short.mp4" next to `beside`, numbered when it exists ("-short-2.mp4")
QString freeName(const QString& dir, const QString& base, const QString& suffix)
{
    QString path = QDir(dir).filePath(base + suffix);
    for (int i = 2; QFileInfo::exists(path); ++i) path = QDir(dir).filePath(QString("%1-%2%3").arg(base).arg(i).arg(suffix));
    return path;
}

QString uniqueTimelineName(const QJsonArray& timelines, const QString& wanted)
{
    const auto taken = [&](const QString& n) {
        return std::any_of(timelines.begin(), timelines.end(), [&](const QJsonValue& v) {
            return v["name"].toString().compare(n, Qt::CaseInsensitive) == 0;
        });
    };
    QString name = wanted;
    for (int i = 2; taken(name); ++i) name = QString("%1 %2").arg(wanted).arg(i);
    return name;
}

double secondsOf(const QJsonValue& v) { return std::round(v.toDouble() * 100) / 100; }

} // namespace

QJsonObject cmdShort(const QJsonObject& a, Context& ctx)
{
    // Progress of one step as a part [from, to) of the whole
    const auto step = [&ctx](double from, double to) {
        Context sub;
        sub.mcp = ctx.mcp;
        sub.partId = ctx.partId;
        if (ctx.progress) sub.progress = [&ctx, from, to](double p) { ctx.progress(from + (to - from) * std::clamp(p, 0.0, 1.0)); };
        return sub;
    };
    const auto run = [](const char* command, const QJsonObject& args, Context c) {
        return execute(*find(command), args, c);
    };

    QStringList files;
    for (const QJsonValue& v : a["files"].toArray()) {
        const QString f = absolute(v.toString());
        if (!QFileInfo(f).isFile()) fail("NOT_FOUND", "no such file: " + f);
        if (isProjectFile(f)) fail("BAD_ARGUMENT", "files are recordings; a project goes into 'project'");
        files << f;
    }
    QString project = a["project"].toString();
    if (!project.isEmpty() && project != QLatin1String(Live::AppProject)) project = absolute(project);
    if (files.isEmpty() && project.isEmpty())
        fail("BAD_ARGUMENT", "give the recordings (files) or a project with the timeline to make the short of");

    ShortPick::Options pick;
    if (a.contains("min_s")) pick.minS = std::clamp(a["min_s"].toDouble(), 1.0, 600.0);
    if (a.contains("max_s")) pick.maxS = std::clamp(a["max_s"].toDouble(), 1.0, 600.0);
    if (pick.maxS < pick.minS) fail("BAD_ARGUMENT", "max_s must not be below min_s");
    const QString aspect = a.contains("aspect") ? a["aspect"].toString().trimmed() : QString("9:16");
    if (!QStringList{"9:16", "1:1", "4:5"}.contains(aspect)) fail("BAD_ARGUMENT", "aspect must be 9:16, 1:1 or 4:5");
    const QString reframe = a.contains("reframe") ? a["reframe"].toString().trimmed().toLower() : QString("auto");
    if (!QStringList{"auto", "center", "fit"}.contains(reframe)) fail("BAD_ARGUMENT", "reframe must be auto, center or fit");
    const QString captions = a.contains("captions") ? a["captions"].toString().trimmed().toLower() : QString("karaoke");
    if (!QStringList{"karaoke", "word", "off"}.contains(captions))
        fail("BAD_ARGUMENT", "captions must be karaoke, word or off");
    const ShortPlatform* platform = shortPlatform(a.contains("platform") ? a["platform"].toString() : QString("shorts"));
    if (!platform) fail("BAD_ARGUMENT", "platform must be shorts, reels or tiktok");
    if (platform->maxSeconds < pick.maxS) pick.maxS = platform->maxSeconds;
    pick.minS = std::min(pick.minS, pick.maxS);
    const GameHint* game = nullptr;
    if (a.contains("game")) {
        const QString g = a["game"].toString().trimmed();
        if (g.compare("none", Qt::CaseInsensitive) != 0 && !(game = gameHint(g))) {
            QStringList ids{"none"};
            for (const GameHint& h : kGames) ids << h.id;
            fail("BAD_ARGUMENT", "game must be " + ids.join(", ") + " (or leave it out: from the folder or file name)");
        }
    } else if (!files.isEmpty()) {
        game = guessGame(files.first());
    }

    // Where the video goes: given (not replaced unless overwrite), else next to the first recording / the project
    QString out = a["out"].toString();
    if (!out.isEmpty()) {
        out = absolute(out);
        if (QFileInfo(out).suffix().isEmpty()) out += ".mp4";
        if (QFileInfo::exists(out) && !a["overwrite"].toBool())
            fail("EXISTS", "output exists (pass overwrite to replace it): " + out);
    } else {
        const QFileInfo base(!files.isEmpty() ? files.first() : project);
        if (base.fileName() == QLatin1String(Live::AppProject)) fail("BAD_ARGUMENT", "give 'out' for a short of the app's project");
        out = freeName(base.absolutePath(), base.completeBaseName() + "-short", ".mp4");
    }

    QJsonArray done, notes;
    QJsonArray moments;
    QString cutTimeline;

    if (!files.isEmpty()) {
        // 1. The recordings: length, frame rate, sound streams
        QVector<double> durations;
        double fps = 0;
        bool streams = false;
        for (int i = 0; i < files.size(); ++i) {
            const QJsonObject pr = run("probe", {{"file", files[i]}}, step(0.01 * i / files.size(), 0.01 * (i + 1) / files.size()));
            if (!pr["video"].toBool()) fail("BAD_MEDIA", "not a video: " + files[i]);
            durations << pr["seconds"].toDouble();
            if (i == 0) fps = pr["fps"].toDouble();
            streams |= pr["audio_streams"].toArray().size() > 1;
        }
        // 2. The project: a new one next to the video unless one is given (that one gets the new timelines)
        if (project.isEmpty()) {
            const QFileInfo o(out);
            project = freeName(o.absolutePath(), o.completeBaseName(), "." + QString(ProjectFile::Extension));
        }
        if (project != QLatin1String(Live::AppProject) && !QFileInfo::exists(project)) {
            // game: the Mix stream of a multi-stream recording comes in muted (nothing plays twice), master limiter
            QJsonObject n{{"project", project}, {"media", QJsonArray::fromStringList(files)},
                          {"template", game || streams ? "game" : "neutral"}};
            if (fps > 0) n["fps"] = fps;
            run("new", n, step(0.01, 0.02));
            done << QString("new project %1 (template %2)").arg(project, n["template"].toString());
        }
        // 3. The best moments of every recording
        QVector<ShortPick::Moment> candidates;
        for (int i = 0; i < files.size(); ++i) {
            QJsonObject h{{"file", files[i]}, {"project", project}};
            if (game) {
                h["region"] = QString::fromLatin1(game->region);
                h["color"] = QString::fromLatin1(game->color);
                if (game->tolerance > 0) h["tolerance"] = game->tolerance;
            }
            const double f0 = 0.02 + 0.28 * i / files.size(), f1 = 0.02 + 0.28 * (i + 1) / files.size();
            const QJsonObject hl = run("highlights", h, step(f0, f1));
            for (const QJsonValue& c : hl["candidates"].toArray())
                candidates << ShortPick::Moment{i, c["from_s"].toDouble(), c["to_s"].toDouble(), c["score"].toDouble()};
        }
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](const ShortPick::Moment& x, const ShortPick::Moment& y) { return x.score > y.score; });
        done << QString("highlights: %1 candidate%2%3")
                    .arg(candidates.size())
                    .arg(candidates.size() == 1 ? "" : "s")
                    .arg(game ? QString(" (kill feed of %1)").arg(game->id) : QString(" (loud moments)"));
        const QVector<ShortPick::Moment> picked = ShortPick::pick(candidates, durations, pick);
        if (candidates.isEmpty())
            notes << "no highlight found: the short is the end of the recording (where a clip tool saves the moment)";
        // 4. A cut of them in the recordings' shape (the user can rework it), one keep per recording
        const QJsonObject before = run("info", {{"project", project}, {"summary", true}}, step(0.3, 0.3));
        cutTimeline = uniqueTimelineName(before["timelines"].toArray(), T("Short-Schnitt"));
        QJsonArray ops{QJsonObject{{"op", "new_timeline"}, {"name", cutTimeline}}};
        for (int i = 0; i < files.size(); ++i) {
            QJsonArray ranges;
            for (const ShortPick::Moment& m : picked) {
                if (m.file != i) continue;
                ranges << QJsonArray{QString("%1s").arg(m.from, 0, 'f', 3), QString("%1s").arg(m.to, 0, 'f', 3)};
                moments << QJsonObject{{"file", files[i]}, {"from_s", secondsOf(m.from)}, {"to_s", secondsOf(m.to)}};
            }
            if (!ranges.isEmpty()) ops << QJsonObject{{"op", "keep"}, {"media", files[i]}, {"ranges", ranges}};
        }
        run("edit", {{"project", project}, {"ops", ops}}, step(0.3, 0.32));
        done << QString("cut: %1 moment%2, %3 s (timeline '%4')")
                    .arg(picked.size())
                    .arg(picked.size() == 1 ? "" : "s")
                    .arg(secondsOf(ShortPick::total(picked)))
                    .arg(cutTimeline);
    } else {
        // An existing timeline (default: the open one) is the cut
        const QJsonObject info = run("info", {{"project", project}, {"summary", true}, {"timeline", a["timeline"]}}, step(0, 0.01));
        cutTimeline = info["timeline"]["name"].toString();
        const double length = info["timeline"]["length_s"].toDouble();
        if (length <= 0) fail("EMPTY", "the timeline '" + cutTimeline + "' is empty");
        if (length > pick.maxS + 0.5)
            notes << QString("the timeline '%1' is %2 s, longer than max_s %3 s: it is used whole (cut it first, or "
                             "give the recordings as files to pick the best moments)")
                         .arg(cutTimeline)
                         .arg(secondsOf(length))
                         .arg(pick.maxS);
    }

    // 5. The vertical copy, every clip following its subject
    const QJsonObject now = run("info", {{"project", project}, {"summary", true}}, step(0.32, 0.32));
    const QString shortTimeline = uniqueTimelineName(now["timelines"].toArray(), T("Short"));
    run("edit",
        {{"project", project},
         {"ops", QJsonArray{QJsonObject{{"op", "new_timeline"}, {"name", shortTimeline}, {"from", cutTimeline},
                                        {"aspect", aspect}, {"reframe", reframe}, {"open", false}}}}},
        step(0.32, 0.4));
    done << QString("timeline '%1' %2, reframe %3").arg(shortTimeline, aspect, reframe);

    // 6. Word captions from the speech, when whisper is there (never installed from here)
    bool burn = false;
    if (captions != QLatin1String("off")) {
        if (Extensions::whisperProgram().isEmpty() || Extensions::installedWhisperModels().isEmpty()) {
            notes << "no captions: speech recognition (whisper) is not installed. Ask the user, then `schneidi-cli "
                     "extensions --install whisper,small` (190 MB; whisper,turbo 574 MB is better) and run again "
                     "(or captions:'off')";
        } else {
            QJsonObject t{{"source", project}, {"timeline", shortTimeline}, {"apply", true}, {"captions", captions}};
            if (a.contains("language")) t["language"] = a["language"];
            try {
                const QJsonObject tr = run("transcribe", t, step(0.4, 0.6));
                burn = true;
                done << QString("%1 captions (%2 segments)").arg(captions).arg(tr["segments"].toArray().size());
            } catch (const Error& e) {
                if (e.code != QLatin1String("NO_SPEECH")) throw;
                notes << "no captions: no speech in the short";
            }
        }
    }

    // 7. Check for the platform and apply its safe fixes (master limiter, fader to the loudness target)
    const QString platformId = QString::fromLatin1(platform->alias);
    const QJsonObject check = run("check", {{"source", project}, {"timeline", shortTimeline}, {"platform", platformId}},
                                  step(0.6, 0.7));
    QJsonObject checked{{"errors", check["errors"]}, {"warnings", check["warnings"]}};
    if (const QJsonObject fix = check["fix"].toObject(); !fix.isEmpty()) {
        run("edit", {{"project", project}, {"ops", fix["ops"]}}, step(0.7, 0.7));
        checked["fixed"] = fix["what"];
        done << "check fixed: " + fix["what"].toString();
    }
    QJsonArray left; // what the fix does not solve, for the caller to look at
    static const QStringList solved{"clipping", "true_peak", "loudness", "encoder_quality"};
    for (const QJsonValue& v : check["issues"].toArray())
        if (!solved.contains(v["check"].toString())) left << v;
    checked["issues"] = left;

    // 8. Render with the platform's preset
    QJsonObject r{{"project", project}, {"timeline", shortTimeline}, {"out", out},
                  {"preset", QString::fromLatin1(platform->id)}, {"overwrite", a["overwrite"].toBool()}};
    if (burn) r["subtitles"] = "burn";
    if (aspect != QLatin1String("9:16")) r["size"] = aspect == QLatin1String("1:1") ? "1080x1080" : "1080x1350";
    for (const char* k : {"threads", "encoder"})
        if (a.contains(k)) r[k] = a[k];
    const QJsonObject rendered = run("render", r, step(0.7, 1));
    done << QString("render %1 (%2)").arg(rendered["preset"].toString(), rendered["encoder"].toString());

    const QJsonObject result = run("info", {{"project", project}, {"summary", true}, {"timeline", shortTimeline}}, step(1, 1));
    QJsonObject o{{"out", out},
                  {"length_s", secondsOf(result["timeline"]["length_s"])},
                  {"width", rendered["width"]},
                  {"height", rendered["height"]},
                  {"project", project},
                  {"timeline", shortTimeline},
                  {"cut_timeline", cutTimeline},
                  {"platform", QString::fromLatin1(platform->name)},
                  {"done", done},
                  {"check", checked},
                  {"render_s", rendered["render_s"]}};
    if (!moments.isEmpty()) o["moments"] = moments;
    if (rendered.contains("encoder")) o["encoder"] = rendered["encoder"];
    if (rendered.contains("encoder_hint")) notes << rendered["encoder_hint"];
    if (!notes.isEmpty()) o["notes"] = notes;
    o["next"] = "look at the result: `frames` of the out file (sheet) and `check` on it";
    return o;
}

} // namespace Cli::detail
