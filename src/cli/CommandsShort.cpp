// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `short`: from recordings (or a timeline) to a finished vertical short in one call. Only strings the
// other commands together (probe, new, `direct` recipe short + apply, transcribe, check + its fix, render), each
// through execute() like a caller would: live in the app with "@app"; the moments and the cut are `direct`'s
// (cli/CommandsDirect.cpp, core/ShortPick).
#include "cli/AnalysisDetail.h"
#include "cli/CommandsDetail.h"
#include "cli/DirectDetail.h"
#include "cli/Live.h"

#include "core/GameHud.h"
#include "core/I18n.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/SfxPlace.h"
#include "core/SfxSynth.h"
#include "core/ShortPick.h"
#include "core/StreamPresets.h"
#include "engine/Extensions.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSize>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Cli::detail {

namespace {

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

namespace {
// Sample peak of a timeline's mix (dBFS); nullopt = silent or not measurable
std::optional<double> soundPeakDb(const QString& project, const QString& timeline, Context c)
{
    try {
        const QJsonObject l = execute(*find("loudness"), {{"source", project}, {"timeline", timeline}}, c);
        const QJsonValue v = l["loudness"]["sample_peak_db"];
        if (v.isDouble() && v.toDouble() > -60) return v.toDouble();
    } catch (const Error&) {
    }
    return std::nullopt;
}
} // namespace

QJsonArray shortFinishOps(const QString& project, const QString& timeline, const ShortFinish& f, Context& ctx,
                          QJsonArray* done, QJsonArray* notes, QJsonArray* noteKeys)
{
    const auto note = [&](const char* key) {
        *notes << Direction::noteText(key);
        if (noteKeys) *noteKeys << QString(key);
    };
    const auto run = [&ctx](const char* command, const QJsonObject& args) {
        Context sub;
        sub.cancel = ctx.cancel;
        sub.progress = ctx.progress;
        return execute(*find(command), args, sub);
    };
    QJsonArray ops;
    // Word captions from the speech, when whisper is there (never installed from here)
    if (f.captions != QLatin1String("off")) {
        if (Extensions::whisperProgram().isEmpty() || Extensions::installedWhisperModels().isEmpty()) {
            note("no_captions_whisper");
        } else {
            QJsonObject t{{"source", project}, {"timeline", timeline}, {"words", true}};
            if (!f.language.isEmpty()) t["language"] = f.language;
            QJsonArray words;
            int segments = 0;
            try {
                const QJsonObject tr = run("transcribe", t);
                segments = int(tr["segments"].toArray().size());
                // Only words that start inside the timeline (as transcribe apply did)
                const int end = run("info", {{"project", project}, {"timeline", timeline}, {"summary", true}})["timeline"]
                                    .toObject()["length"].toInt();
                for (const QJsonValue& w : tr["words"].toArray())
                    if (end <= 0 || w["from"].toInt() < end) words << w;
            } catch (const Error& e) {
                if (e.code != QLatin1String("NO_SPEECH")) throw;
            }
            if (words.isEmpty()) {
                note("no_captions_speech");
            } else {
                ops << QJsonObject{{"op", "captions"}, {"style", f.captions}, {"words", words}};
                *done << QString("%1 captions (%2 segments)").arg(f.captions).arg(segments);
            }
        }
    }
    // Effect sounds (sfx:'mild' only): a whoosh on the hard cuts, a hit on the kills, sparingly (op 'sfx' rules)
    if (f.sfx == QLatin1String("mild")) {
        // The hits first (they matter most), the whooshes in the room left (each kind has its own cap per minute)
        if (!f.game.isEmpty()) {
            // The hits 14 dB under the game sound's peak (without game sound: level mild)
            QJsonObject hit{{"op", "sfx"}, {"on", "kills"}, {"kind", "hit"}, {"level", "mild"}, {"game", f.game}};
            Context peak;
            peak.cancel = ctx.cancel;
            hit["volume_db"] = SfxPlace::underPeakDb(soundPeakDb(project, timeline, peak),
                                                     SfxSynth::peakLevelDb(SfxSynth::Kind::Hit));
            ops << hit;
        }
        ops << QJsonObject{{"op", "sfx"}, {"on", "cuts"}, {"kind", "whoosh"}, {"level", "mild"}};
        // The kill feed read here (a measure the app's edit then only places): `_kills`
        Context sub;
        sub.cancel = ctx.cancel;
        ops = prepareSfx({{"project", project}, {"timeline", timeline}, {"ops", ops}}, sub)["ops"].toArray();
    }
    failIfCanceled();
    return ops;
}

bool shortFinishEdit(const QString& project, const QString& timeline, const QJsonArray& ops, Context& ctx,
                     QJsonArray* done)
{
    if (ops.isEmpty()) return false;
    Context sub;
    sub.cancel = ctx.cancel;
    const QJsonObject e = execute(*find("edit"), {{"project", project}, {"timeline", timeline}, {"ops", ops}}, sub);
    bool captions = false, sfx = false;
    for (const QJsonValue& op : ops) {
        captions |= op["op"].toString() == QLatin1String("captions");
        sfx |= op["op"].toString() == QLatin1String("sfx");
    }
    if (sfx) {
        int placed = 0;
        for (const QJsonValue& n : e["notes"].toArray()) placed += int(n["sfx"]["placed"].toArray().size());
        *done << QString("sfx mild: %1 sound%2").arg(placed).arg(placed == 1 ? "" : "s");
    }
    return captions;
}

bool shortFinish(const QString& project, const QString& timeline, const ShortFinish& f, Context& ctx, QJsonArray* done,
                 QJsonArray* notes)
{
    return shortFinishEdit(project, timeline, shortFinishOps(project, timeline, f, ctx, done, notes), ctx, done);
}

QJsonObject cmdShort(const QJsonObject& a, Context& ctx)
{
    // Progress of one step as a part [from, to) of the whole
    const auto step = [&ctx](double from, double to) {
        Context sub;
        sub.mcp = ctx.mcp;
        sub.partId = ctx.partId;
        sub.stage = ctx.stage; // each step names itself (execute)
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
        if (!files.contains(f)) files << f; // the same recording twice is one (as in `direct`)
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
    // Hook: the best moment first (default; the rest in their order), or all in the order they happened
    const QString hook = a.contains("hook") ? a["hook"].toString().trimmed().toLower() : QString("best");
    if (!QStringList{"best", "chronological"}.contains(hook)) fail("BAD_ARGUMENT", "hook must be best or chronological");
    // Effect sounds: none unless asked for (never by itself on every cut)
    const QString sfx = a.contains("sfx") ? a["sfx"].toString().trimmed().toLower() : QString("off");
    if (!QStringList{"off", "mild"}.contains(sfx)) fail("BAD_ARGUMENT", "sfx must be off or mild");
    const ShortPlatform* platform = shortPlatform(a.contains("for") ? a["for"].toString() : QString("shorts"));
    if (!platform) fail("BAD_ARGUMENT", "for must be shorts, reels or tiktok (the platform)");
    if (platform->maxSeconds < pick.maxS) pick.maxS = platform->maxSeconds;
    pick.minS = std::min(pick.minS, pick.maxS);
    // Sound of the recordings: game (default: only the game sound of multi-stream recordings), all, or streams
    const QString streamChoice = a.contains("audio_stream") ? a["audio_stream"].toVariant().toString().trimmed() : QString("game");
    const GameHud::Game* game = nullptr;
    const bool noGame = a.contains("game") && a["game"].toString().trimmed().compare("none", Qt::CaseInsensitive) == 0;
    if (a.contains("game") && !noGame) {
        if (!(game = GameHud::find(a["game"].toString()))) {
            QStringList ids{"none"};
            for (const GameHud::Game& h : GameHud::games()) ids << h.id;
            fail("BAD_ARGUMENT", "game must be " + ids.join(", ") + " (or leave it out: from the folder or file name)");
        }
    } else if (!files.isEmpty() && !noGame) {
        game = GameHud::guess(files.first());
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
    QJsonArray unused; // recordings without a moment in the short, and why
    QString cutTimeline;

    QString shortTimeline;
    QJsonArray madeNotes; // notes of the edit that made the vertical copy (what the reframe did with the kill feed)
    if (!files.isEmpty()) {
        // 1. The recordings: a picture, the frame rate, several sound streams
        double fps = 0;
        bool streams = false;
        for (int i = 0; i < files.size(); ++i) {
            const QJsonObject pr = run("probe", {{"file", files[i]}}, step(0.01 * i / files.size(), 0.01 * (i + 1) / files.size()));
            if (!pr["video"].toBool()) fail("BAD_MEDIA", "not a video: " + files[i]);
            if (i == 0) fps = pr["fps"].toDouble();
            streams |= pr["audio_streams"].toArray().size() > 1;
        }
        // 2. The project: a new one next to the video unless one is given (that one gets the new timelines)
        const QFileInfo o(out);
        const auto freeProject = [&] { return freeName(o.absolutePath(), o.completeBaseName(), "." + QString(ProjectFile::Extension)); };
        const bool chosen = project.isEmpty(); // a free name of our own: always a new project, never one made meanwhile
        if (chosen) project = freeProject();
        bool created = false;
        if (project != QLatin1String(Live::AppProject) && (chosen || !QFileInfo::exists(project))) {
            // game: the Mix stream of a multi-stream recording comes in muted (nothing plays twice), master limiter
            QJsonObject n{{"media", QJsonArray::fromStringList(files)}, {"template", game || streams ? "game" : "neutral"}};
            if (fps > 0) n["fps"] = fps;
            // The free name can be taken between choosing and creating (another short of the same video): `new`
            // answers EXISTS, the next free name is tried (a few times)
            for (int attempt = 0;; ++attempt) {
                n["project"] = project;
                try {
                    run("new", n, step(0.01, 0.02));
                    break;
                } catch (const Error& e) {
                    if (!chosen || e.code != QLatin1String("EXISTS") || attempt >= 8) throw;
                    project = freeProject();
                }
            }
            created = true;
            done << QString("new project %1 (template %2)").arg(project, n["template"].toString());
        }
        // 3. The best moments of every recording: `direct` recipe short (the highlights, ShortPick)
        QJsonObject options;
        for (const char* k : {"min_s", "max_s", "for", "hook", "aspect", "reframe", "audio_stream", "accents"})
            if (a.contains(k)) options[k] = a[k];
        options["for"] = QString::fromLatin1(platform->alias);
        if (game) options["game"] = game->id;
        else if (noGame) options["game"] = "none";
        CollectedMoments found;
        Context suggestCtx = step(0.02, 0.3);
        const Direction::Session session = directSuggest(
            {{"recipe", "short"}, {"files", QJsonArray::fromStringList(files)}, {"project", project}, {"options", options}},
            suggestCtx, &found);
        QVector<int> candidates(files.size(), 0); // highlight candidates per recording
        for (const DirectMoment& m : found.highlights) ++candidates[m.file];
        const int count = int(found.highlights.size());
        const int withSome = int(std::count_if(candidates.begin(), candidates.end(), [](int n) { return n > 0; }));
        done << QString("highlights: %1 candidate%2%3 in %4 of %5 recording%6")
                    .arg(count)
                    .arg(count == 1 ? "" : "s")
                    .arg(game ? QString(" (kill feed of %1)").arg(game->id) : QString(" (loud moments)"))
                    .arg(withSome)
                    .arg(files.size())
                    .arg(files.size() == 1 ? "" : "s");
        if (count == 0) {
            if (found.extra.isEmpty())
                notes << "no highlight found: the short is the end of the recording (where a clip tool saves the moment)";
            else
                notes << "no highlight found: the short is made of sound events and picture changes";
        }
        // Why a recording has no moment in the short: none found in it, or its moments did not fit (better ones of
        // the others filled max_s first)
        double maxS = session.target;
        for (int i = 0; i < files.size(); ++i) {
            if (std::any_of(session.suggestions.begin(), session.suggestions.end(),
                            [&](const Direction::Suggestion& x) { return x.on && x.file == files[i]; }))
                continue;
            QJsonObject u{{"file", files[i]}, {"candidates", candidates[i]}};
            const QString name = QFileInfo(files[i]).fileName();
            if (candidates[i] == 0) {
                u["reason"] = "no_moment";
                u["message"] = game ? QString("no kill of the player found in its kill feed (%1)").arg(game->id)
                                    : QString("no loud moment found");
            } else {
                u["reason"] = "length";
                u["message"] = QString("%1 moment%2 found, none fit: the better ones filled the short to max_s %3 s")
                                   .arg(candidates[i])
                                   .arg(candidates[i] == 1 ? "" : "s")
                                   .arg(maxS);
            }
            notes << QString("%1 not used: %2").arg(name, u["message"].toString());
            unused << u;
        }
        // 4. Applied: a cut in the recordings' shape (one keep per recording, the best moment first as the hook)
        // and its vertical copy, every clip following its subject - one edit
        Context applyCtx = step(0.3, 0.4);
        const QJsonObject applied = directApply(session, {{"on", "all"}},
                                                {{"project", project}, {"_created", created}, {"_no_check", true}},
                                                applyCtx);
        cutTimeline = applied["cut_timeline"].toString();
        shortTimeline = applied["timeline"].toString();
        madeNotes = applied["edit_notes"].toArray();
        for (const QJsonValue& v : applied["done"].toArray())
            if (!v.toString().startsWith("cut: ")) done << v;
        for (const QJsonValue& m : applied["moments"].toArray())
            moments << QJsonObject{{"file", m["file"]}, {"from_s", m["from_s"]}, {"to_s", m["to_s"]}};
        if (hook == QLatin1String("best") && moments.size() > 1)
            done << QString("hook: the best moment first (%1 s of %2)")
                        .arg(moments[0].toObject()["from_s"].toDouble())
                        .arg(QFileInfo(moments[0].toObject()["file"].toString()).fileName());
        for (const QJsonValue& v : applied["done"].toArray())
            if (v.toString().startsWith("cut: ")) done << v;
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
        // 5. The vertical copy, every clip following its subject
        const QJsonObject now = run("info", {{"project", project}, {"summary", true}}, step(0.32, 0.32));
        shortTimeline = uniqueTimelineName(now["timelines"].toArray(), T("Short"));
        // Game footage keeps its kill feed readable at the top (reframe keep:'auto'; the game given here counts)
        QJsonObject copy{{"op", "new_timeline"}, {"name", shortTimeline}, {"from", cutTimeline},
                         {"aspect", aspect}, {"reframe", reframe}, {"open", false}};
        if (game) copy["game"] = game->id;
        if (noGame) copy["keep"] = "none";
        const QJsonObject made = run("edit", {{"project", project}, {"ops", QJsonArray{copy}}}, step(0.32, 0.4));
        madeNotes = made["notes"].toArray();
    }
    // What the reframe did with the kill feed, from its notes: copies made (following their lines or the whole
    // region), or none (no line in the clips)
    const auto noted = [&madeNotes](const QString& start, const QString& with = {}) {
        return std::any_of(madeNotes.begin(), madeNotes.end(), [&](const QJsonValue& n) {
            return n.toString().startsWith(start) && n.toString().contains(with);
        });
    };
    QString feedText;
    if (noted("kept readable", ": killfeed (")) {
        feedText = noted("the kill feed copy follows its lines") ? ", kill feed kept at the top (shown with its lines)"
                                                                 : ", kill feed kept at the top";
        if (noted("no kill feed line found in clips")) feedText += " (whole region where no line was recognized)";
    } else if (noted("no kill feed line in clips")) {
        feedText = ", no kill feed line in the clips: no kill feed copy";
    }
    done << QString("timeline '%1' %2, reframe %3%4").arg(shortTimeline, aspect, reframe, feedText);

    // 6. Word captions (whisper) and effect sounds (sfx:'mild'), one edit
    ShortFinish finish{captions, sfx, game ? game->id : QString(), a["language"].toString()};
    Context finishCtx = step(0.4, 0.6);
    const bool burn = shortFinish(project, shortTimeline, finish, finishCtx, &done, &notes);

    // 7. Check for the platform and apply its safe fixes (master limiter, fader to the loudness target)
    const QString platformId = QString::fromLatin1(platform->alias);
    const QJsonObject check = run("check", {{"source", project}, {"timeline", shortTimeline}, {"for", platformId}},
                                  step(0.6, 0.7));
    QJsonObject checked;
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
    int errors = 0, warnings = 0;
    for (const QJsonValue& v : left) {
        errors += v["severity"].toString() == QLatin1String("error");
        warnings += v["severity"].toString() == QLatin1String("warning");
    }
    checked["errors"] = errors;
    checked["warnings"] = warnings;

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
    if (!unused.isEmpty()) o["unused"] = unused;
    if (rendered.contains("encoder")) o["encoder"] = rendered["encoder"];
    for (const char* k : {"threads", "threads_reason"}) // how many render threads, fewer when little RAM was free
        if (rendered.contains(k)) o[k] = rendered[k];
    if (rendered.contains("encoder_hint")) notes << rendered["encoder_hint"];
    if (!notes.isEmpty()) o["notes"] = notes;
    o["next"] = "look at the result: `frames` of the out file (sheet) and `check` on it";
    return o;
}

} // namespace Cli::detail
