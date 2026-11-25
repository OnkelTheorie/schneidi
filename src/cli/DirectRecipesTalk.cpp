// SPDX-License-Identifier: AGPL-3.0-only
// `direct` recipes on what other commands find (cli/CommandsDirect.cpp has short and highlights):
// - talk: the pauses and filler words `cut_pauses` finds on a timeline (a dry run measures them); applying cuts the
//   chosen ranges out of a copy of that timeline (one edit: new_timeline from it + cut_pauses ranges).
// - template: the shots `templates auto:true` chooses for a content template's clip slots (the material index is built
//   while suggesting, never when applying); applying fills the slots with the chosen values.
// - script: the takes `rough_cut` finds for each sentence of a script (a dry run; whisper for files without a
//   transcript, here and not in the app); applying puts the chosen takes one after another on a new timeline.
#include "cli/AnalysisDetail.h"
#include "cli/DirectDetail.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"

#include "core/AppPaths.h"
#include "core/GameHud.h"
#include "core/I18n.h"
#include "engine/AnalysisCache.h"
#include "core/LiveHost.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "core/Vocabulary.h"
#include "engine/Extensions.h"
#include "core/TranscriptCut.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace Cli::detail {

namespace {

QJsonObject runSub(const char* command, const QJsonObject& args, Context& ctx)
{
    Context sub;
    sub.cancel = ctx.cancel;
    sub.progress = ctx.progress;
    sub.stage = ctx.stage;
    return execute(*find(command), args, sub);
}

QString seconds3(double s) { return QString("%1s").arg(s, 0, 'f', 3); }

// The project a recipe on a timeline works on: given, else the app's (inside it or a background job of it)
QString projectOf(Direction::Session& s)
{
    if (s.project.isEmpty() && (Live::host() || Live::appCopy())) s.project = Live::AppProject;
    if (s.project.isEmpty())
        fail("NO_MATERIAL", s.recipe == QLatin1String("script")     ? QString("recipe script needs material: give files (recordings) or a project")
                            : s.recipe == QLatin1String("template") ? QString("recipe template needs options.template (a template id) "
                                                                              "or a project with its own fields")
                                                                    : QString("recipe %1 works on a project: give project (and its timeline)").arg(s.recipe));
    return s.project;
}

Direction::Material materialOf(const QString& file)
{
    Direction::Material m;
    const QFileInfo fi(file);
    m.file = fi.absoluteFilePath();
    m.size = fi.size();
    m.modified = fi.lastModified().toMSecsSinceEpoch();
    m.analysis = {{"sound", "none"}, {"picture", "none"}, {"events", "none"}, {"speech", "none"}};
    // Its length (adjust keeps moved edges inside it)
    try {
        const MediaSource src = openMedia({{"file", m.file}});
        m.length = seconds(src.info.length, src.format);
        m.hasVideo = src.info.hasVideo && !src.info.isImage;
        m.hasAudio = src.info.hasAudio;
    } catch (const Error&) {
        // unreadable: length unknown (0)
    }
    return m;
}

QJsonObject noteObject(const QJsonArray& notes, const char* key)
{
    for (const QJsonValue& n : notes)
        if (n.toObject().contains(key)) return n.toObject().value(key).toObject();
    return {};
}

// Verbatim word transcripts (fillers written) of the recordings on a timeline that have none, per file and sound
// stream: {file: [{stream, language, words:[[from_ms, to_ms, text]…]}]} for the _transcripts op. Each through the
// analysis cache ("speech-words"), whisper only for what is not there. Empty without whisper.
QJsonObject verbatimTranscripts(const QString& project, int timeline, const QString& language, Context& ctx)
{
    QJsonObject out;
    if (Extensions::whisperProgram().isEmpty() || Extensions::installedWhisperModels().isEmpty()) return out;
    Session view(project);
    view.selectTimeline(timeline);
    for (const auto& [file, stream] : TranscriptCut::missingTranscripts(view.project.timeline(), view.project.media())) {
        const std::optional<QJsonObject> one = streamTranscript(file, stream, language, true, ctx);
        if (!one) continue; // no speech, an unreadable stream: no fillers from it
        QJsonArray list = out[file].toArray();
        list << *one;
        out[file] = list;
    }
    return out;
}

} // namespace

std::optional<QJsonObject> streamTranscript(const QString& file, int stream, const QString& language, bool verbatim,
                                            Context& ctx, const QStringList& vocabulary, const QString& model)
{
    if (!QFileInfo::exists(file)) return std::nullopt;
    QJsonObject keyOptions{{"language", language}, {"stream", stream}};
    keyOptions["verbatim"] = verbatim; // (the key of the verbatim ones as before)
    // Another word list is another transcript (no list: the key as before)
    if (const QString id = Vocabulary::cacheId(Vocabulary::combine(Vocabulary::stored(), vocabulary)); !id.isEmpty())
        keyOptions["vocabulary"] = id;
    if (!model.isEmpty()) keyOptions["model"] = QFileInfo(model).fileName(); // (the default model: the key as before)
    const AnalysisCache::Key key = AnalysisCache::key("speech-words", file, keyOptions, 1);
    if (const auto hit = AnalysisCache::get(key)) return *hit;
    QJsonObject t{{"source", file}, {"words", true}, {"audio_stream", QString::number(stream)}};
    if (!vocabulary.isEmpty()) t["vocabulary"] = QJsonArray::fromStringList(vocabulary);
    if (!model.isEmpty()) t["model"] = model;
    if (verbatim) t["verbatim"] = true;
    if (!language.isEmpty()) t["language"] = language;
    QJsonObject r;
    try {
        r = runSub("transcribe", t, ctx);
    } catch (const Error& e) {
        if (e.code == QLatin1String("CANCELED")) throw;
        if (e.code != QLatin1String("NO_SPEECH")) return std::nullopt; // an unreadable stream: asked again next time
    }
    const double fps = r["fps"].toDouble(25);
    QJsonArray words;
    for (const QJsonValue& w : r["words"].toArray())
        words << QJsonArray{std::round(w["from"].toDouble() / fps * 1000), std::round(w["to"].toDouble() / fps * 1000),
                            w["text"].toString()};
    const QJsonObject one{{"stream", stream}, {"language", r["language"].toString()}, {"words", words}};
    AnalysisCache::put(key, one);
    return one;
}

QString timelineStamp(const QString& project, const QString& timeline)
{
    Session s(project);
    if (!timeline.isEmpty() && timeline != QLatin1String("@current")) s.selectTimeline(timeline);
    QByteArray bytes = QByteArray::number(s.project.currentSequence());
    const Timeline& tl = s.project.timeline();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            for (const Clip& c : t.clips) bytes += QJsonDocument(ProjectFile::clipJson(c)).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}

// ---------- talk ----------

void suggestTalk(Direction::Session& s, const QJsonObject& a, Context& ctx)
{
    const QString project = projectOf(s);
    if (!a.contains("timeline") && !a["files"].toArray().isEmpty())
        fail("BAD_ARGUMENT", "talk works on a timeline (timeline:'@current' or its name), not on files: put the "
                             "recordings on a timeline first");
    // The timeline by its id from now on: another one opened later (the result of an apply) changes nothing
    {
        Session view(project);
        const QString given = a["timeline"].toString("@current").trimmed();
        if (given != QLatin1String("@current")) view.selectTimeline(given);
        s.timeline = QString::number(view.project.currentSequence());
    }
    const QJsonObject& o = s.options;
    QJsonObject op{{"op", "cut_pauses"}};
    if (o.contains("min_pause_s")) op["min_pause"] = seconds3(o["min_pause_s"].toDouble());
    if (o.contains("pad_s")) op["pad"] = seconds3(o["pad_s"].toDouble());
    if (o.contains("fillers")) op["fillers"] = o["fillers"].toBool();
    if (o.contains("threshold")) op["threshold"] = o["threshold"];
    // tracks as the page sends them (a list) or as text ("A1,A2")
    if (const QJsonValue t = o["tracks"]; (t.isArray() && !t.toArray().isEmpty()) || (t.isString() && !t.toString().trimmed().isEmpty()))
        op["tracks"] = t.isArray() ? QJsonValue(QStringList([&] {
                                         QStringList l;
                                         for (const QJsonValue& v : t.toArray()) l << v.toVariant().toString();
                                         return l;
                                     }()).join(","))
                                   : t;
    QJsonObject e{{"project", project}, {"ops", QJsonArray{op}}, {"dry_run", true}, {"timeline", s.timeline.toInt()}};
    // A dry run measures (on a copy of what the app shows; a background job has no app host, so it measures here)
    QJsonObject r = runSub("edit", e, ctx);
    QJsonObject note = noteObject(r["notes"].toArray(), "cut_pauses");
    // Fillers asked for, but the recordings have no transcript yet (`transcribe` of a project only answers, it keeps
    // nothing): whisper writes them here, verbatim, per recording and sound stream (kept for the next suggestion and
    // put into the project with the apply), then the measuring again
    if (o["fillers"].toBool() && note["filler_words"].toInt(-1) == 0 && !Live::host()) {
        const QJsonObject made = verbatimTranscripts(project, s.timeline.toInt(), o["language"].toString(), ctx);
        failIfCanceled();
        if (!made.isEmpty()) {
            s.options["_transcripts"] = made;
            e["ops"] = QJsonArray{QJsonObject{{"op", "_transcripts"}, {"files", made}}, op};
            r = runSub("edit", e, ctx);
            note = noteObject(r["notes"].toArray(), "cut_pauses");
        }
    }
    Session view(project);
    view.selectTimeline(s.timeline.toInt());
    const double fps = view.format().rate.fps();
    s.options["_timeline_stamp"] = timelineStamp(project, s.timeline);
    s.options["_timeline_name"] = view.project.sequenceName(view.project.currentSequence());
    s.options["_timeline_length_s"] = seconds(TimelineOps::endFrame(view.project.timeline()), view.format());
    // target_lufs: the master's gain measured here (the app's edit only sets it; cutting pauses barely changes the
    // gated loudness)
    if (o.contains("target_lufs") && o["target_lufs"].isDouble()) {
        try {
            const QJsonObject l = runSub("loudness", {{"source", project}, {"timeline", s.timeline.toInt()}}, ctx);
            const QJsonValue lufs = l["loudness"]["integrated_lufs"];
            const double master = view.project.timeline().masterVolumeDb;
            if (lufs.isDouble())
                s.options["_master_db"] = std::round((master + o["target_lufs"].toDouble() - lufs.toDouble()) * 10) / 10;
        } catch (const Error& e) {
            if (e.code == QLatin1String("CANCELED")) throw;
        }
    }
    for (const QString& f : [&] {
             QStringList files;
             const Timeline& tl = view.project.timeline();
             for (const Track& t : tl.audio)
                 for (const Clip& c : t.clips)
                     if (!c.mediaPath.isEmpty() && !files.contains(c.mediaPath)) files << c.mediaPath;
             return files;
         }())
        if (QFileInfo::exists(f)) s.material << materialOf(f);
    for (const QJsonValue& v : note["ranges"].toArray()) {
        Direction::Suggestion x;
        x.n = v["n"].toInt();
        x.kind = v["kind"].toString("pause");
        x.file = "@timeline";
        x.from = v["from"].toDouble() / fps;
        x.to = v["to"].toDouble() / fps;
        QStringList tracks;
        for (const QJsonValue& t : v["tracks"].toArray()) tracks << t.toString();
        x.track = tracks.join(",");
        x.score = v["length_s"].toDouble();
        x.labels << x.kind;
        // A pause as long as it is (the whole quiet run, as cut_pauses measured it), the part cut out as 'cut':
        // "pause of 0.3 s" with min_pause_s 0.7 read like a mistake (videotest 21.11.)
        const double cut = v["length_s"].toDouble();
        const double pause = v["quiet_s"].toDouble(cut);
        x.reason = x.kind == QLatin1String("filler")
                       ? Direction::Reason{"filler", {{"word", v["text"].toString()}}}
                       : Direction::Reason{"pause", {{"s", std::round(pause * 100) / 100}, {"cut", cut}}};
        s.suggestions << x;
        s.order << x.n;
    }
    if (o["room_tone"].toBool()) s.notes << Direction::makeNote("room_tone");
    // The op's notes under fixed keys (the page shows them in the UI language)
    if (note["fillers"].isString())
        s.notes << Direction::makeNote(note["filler_words"].toInt(-1) == 0 ? "no_transcript_fillers" : "fillers_transcript");
    if (!note["never_quiet"].toArray().isEmpty()) {
        QStringList tracks;
        for (const QJsonValue& t : note["never_quiet"].toArray()) tracks << t.toString();
        s.notes << Direction::makeNote("never_quiet", {{"tracks", tracks.join(", ")}});
    } else if (note["note"].isString() && s.suggestions.isEmpty()) {
        s.notes << Direction::makeNote("no_pause");
    } else if (note["note"].isString()) {
        s.notes << Direction::Note{"talk", note["note"].toString()};
    }
}

QJsonObject applyTalk(const Direction::Session& s, const QVector<const Direction::Suggestion*>& list,
                      const QJsonObject& args, Context& ctx)
{
    QString project = args["project"].toString();
    if (project.isEmpty()) project = s.project;
    if (project.isEmpty() && Live::host()) project = Live::AppProject;
    if (!project.isEmpty() && !Live::isAppProject(project)) project = absolute(project);
    if (project.isEmpty()) fail("NO_MATERIAL", "talk works on a project: give project");
    if (timelineStamp(project, s.timeline) != s.options["_timeline_stamp"].toString())
        fail("SESSION_STALE", "the timeline changed since its pauses were found: suggest again");
    const QJsonObject before = runSub("info", {{"project", project}, {"summary", true}}, ctx);
    QStringList names;
    for (const QJsonValue& v : before["timelines"].toArray()) names << v["name"].toString();
    QString name = T("%1 ohne Pausen").arg(s.options["_timeline_name"].toString());
    for (int i = 2; names.contains(name, Qt::CaseInsensitive); ++i)
        name = T("%1 ohne Pausen %2").arg(s.options["_timeline_name"].toString()).arg(i);
    QJsonArray ranges, pauses;
    double removed = 0;
    for (const Direction::Suggestion* x : list) {
        ranges << QJsonArray{seconds3(x->from), seconds3(x->to)};
        pauses << QJsonObject{{"n", x->n}, {"kind", x->kind}, {"from_s", std::round(x->from * 100) / 100},
                              {"to_s", std::round(x->to * 100) / 100}};
        removed += x->to - x->from;
    }
    QJsonObject copy{{"op", "new_timeline"}, {"name", name}, {"from", s.timeline.toInt()}};
    QJsonArray ops{copy, QJsonObject{{"op", "cut_pauses"}, {"ranges", ranges}}};
    // The transcripts whisper wrote while suggesting (fillers) are the recordings' from now on
    if (s.options["_transcripts"].isObject() && !args["plan"].toBool())
        ops << QJsonObject{{"op", "_transcripts"}, {"files", s.options["_transcripts"]}};
    QJsonArray done, notes, noteKeys;
    if (s.options["_master_db"].isDouble()) // measured when suggesting (never on the app's UI thread)
        ops << QJsonObject{{"op", "mixer"}, {"track", "master"}, {"volume_db", s.options["_master_db"]}};
    if (s.options["room_tone"].toBool()) {
        notes << Direction::noteText("room_tone_apply");
        noteKeys << QString("room_tone_apply");
    }
    if (!args["plan"].toBool()) ops << QJsonObject{{"op", "_direction"}, {"session", Direction::toJson(s)}};
    QJsonObject e{{"project", project}, {"ops", ops}};
    if (args["plan"].toBool()) e["plan"] = true;
    if (args["dry_run"].toBool()) e["dry_run"] = true;
    const QJsonObject edited = runSub("edit", e, ctx);
    if (args["plan"].toBool()) return {{"project", project}, {"plan", edited["plan"]}, {"session", s.id}};
    done << QString("cut: %1 pause%2 and filler%2, %3 s out (timeline '%4')")
                .arg(list.size())
                .arg(list.size() == 1 ? "" : "s")
                .arg(std::round(removed * 100) / 100)
                .arg(name);
    QJsonObject out{{"project", project}, {"timeline", name}, {"pauses", pauses}, {"removed_s", std::round(removed * 100) / 100},
                    {"length_s", edited["timeline"]["length_s"]}, {"done", done}, {"session", s.id},
                    {"next", QString("look at it (`frames`, play it), then `render` timeline '%1'").arg(name)}};
    if (!notes.isEmpty()) out["notes"] = notes;
    if (!noteKeys.isEmpty()) out["note_keys"] = noteKeys;
    if (edited.contains("live")) out["live"] = true;
    return out;
}

// ---------- template ----------

namespace {

// How strongly a slot asks for a highlight (its hint or label): the best moment, a strong opener, the ending, any
int slotWish(const QJsonObject& slot)
{
    const QString t = (slot["hint"].toString() + " " + slot["label"].toString()).toLower();
    const auto has = [&](std::initializer_list<const char*> words) {
        return std::any_of(words.begin(), words.end(), [&](const char* w) { return t.contains(QLatin1String(w)); });
    };
    if (has({"best", "beste", "drop", "climax", "höhepunkt", "hoehepunkt"})) return 3;
    if (has({"strong", "stark", "opener", "hook", "opening"})) return 2;
    if (has({"ending", "end", "finale", "schluss", "outro"})) return 1;
    return 0;
}

// The length a moment gets in a slot: a fixed slot its own length, a flexible one the moment's within min_s/max_s
double slotSeconds(const QJsonObject& slot, double momentS)
{
    double fixed = 0;
    for (const QJsonValue& i : slot["instances"].toArray()) fixed = std::max(fixed, i["length_s"].toDouble());
    if (slot["length"].toString() == QLatin1String("fixed") && fixed > 0) return fixed;
    double l = momentS;
    if (slot["max_s"].toDouble() > 0) l = std::min(l, slot["max_s"].toDouble());
    if (slot["min_s"].toDouble() > 0) l = std::max(l, slot["min_s"].toDouble());
    return l > 0 ? l : fixed;
}

} // namespace

void suggestTemplate(Direction::Session& s, const QStringList& files, Context& ctx)
{
    const QString tmpl = s.options["template"].toString().trimmed();
    QJsonObject t{{"auto", true}, {"dry_run", true}, {"values", QJsonObject{}}};
    if (!files.isEmpty()) t["files"] = QJsonArray::fromStringList(files);
    if (!tmpl.isEmpty()) {
        t["fill"] = tmpl;
    } else {
        // The project's own slots (it is a template, or a project made of one)
        t["project"] = projectOf(s);
    }
    const QJsonObject r = runSub("templates", t, ctx);
    QStringList used;
    for (const QJsonValue& p : r["auto"].toArray())
        if (p["file"].isString() && !used.contains(p["file"].toString())) used << p["file"].toString();
    const QStringList material = files.isEmpty() ? used : files;
    for (const QString& f : material) s.material << materialOf(absolute(f));
    // The slots as the template defines them (length, hint)
    QHash<QString, QJsonObject> slotInfo;
    for (const QJsonValue& v : runSub("templates", {{"info", tmpl.isEmpty() ? s.project : tmpl}}, ctx)["slots"].toArray())
        slotInfo.insert(v["id"].toString(), v.toObject());

    // The moments of the material (kills, loud, laughter … as `direct` highlights finds them): a clip field gets a
    // moment, not the first seconds of a recording. The best moments, each recording once while there are others
    QVector<DirectMoment> moments;
    if (!material.isEmpty()) {
        QString game = s.options["game"].toString().trimmed();
        if (game.isEmpty()) game = GameHud::guess(material.first()) ? GameHud::guess(material.first())->id : QString("none");
        else if (game.compare("none", Qt::CaseInsensitive) != 0 && !GameHud::find(game))
            fail("BAD_ARGUMENT", "game must be cs2, valorant or none");
        Context sub;
        sub.cancel = ctx.cancel;
        const CollectedMoments found = collectMoments(material, {}, game, true, sub);
        failIfCanceled();
        moments = found.highlights.isEmpty() ? found.extra : found.highlights;
        std::stable_sort(moments.begin(), moments.end(),
                         [](const DirectMoment& a, const DirectMoment& b) { return a.score > b.score; });
    }
    QVector<QJsonObject> picks;
    for (const QJsonValue& p : r["auto"].toArray()) picks << p.toObject();
    // The slots in the order they want a highlight (the best moment first), else in their own order
    QVector<int> byWish(picks.size());
    std::iota(byWish.begin(), byWish.end(), 0);
    std::stable_sort(byWish.begin(), byWish.end(), [&](int a, int b) {
        return slotWish(slotInfo.value(picks[a]["slot"].toString())) > slotWish(slotInfo.value(picks[b]["slot"].toString()));
    });
    struct Range {
        QString file;
        double from, to;
    };
    QVector<Range> taken;
    QVector<int> momentOf(picks.size(), -1);
    QVector<Range> rangeOf(picks.size());
    const auto place = [&](int slot, int k) { // the slot's window around the moment's action, inside the recording
        const DirectMoment& m = moments[k];
        const QString file = s.material.value(m.file).file;
        const double length = s.material.value(m.file).length;
        const double want = slotSeconds(slotInfo.value(picks[slot]["slot"].toString()), m.to - m.from);
        const double action = m.first >= 0 ? m.first : m.from;
        const double lead = std::min(1.0, want * 0.3);
        double from = std::max(0.0, std::max(m.from, action - lead));
        if (from + want > m.to) from = std::max(0.0, std::min(from, m.to - want)); // more of the moment, less after it
        double to = from + want;
        if (length > 0 && to > length) {
            to = length;
            from = std::max(0.0, to - want);
        }
        return Range{file, from, to};
    };
    for (bool again : {false, true}) // first each recording once, then a second moment of one where it does not overlap
        for (int slot : byWish) {
            if (momentOf[slot] >= 0) continue;
            for (int k = 0; k < moments.size(); ++k) {
                if (std::find(momentOf.begin(), momentOf.end(), k) != momentOf.end()) continue;
                const Range w = place(slot, k);
                const bool sameFile = std::any_of(taken.begin(), taken.end(), [&](const Range& x) { return x.file == w.file; });
                const bool overlaps = std::any_of(taken.begin(), taken.end(), [&](const Range& x) {
                    return x.file == w.file && w.from < x.to + 1.0 && x.from < w.to + 1.0;
                });
                if (overlaps || (sameFile && !again)) continue;
                momentOf[slot] = k;
                rangeOf[slot] = w;
                taken << w;
                break;
            }
        }

    int fromShots = 0;
    for (int i = 0; i < picks.size(); ++i) {
        const QJsonObject& p = picks[i];
        Direction::Suggestion x;
        x.kind = "slot";
        x.slot = p["slot"].toString();
        x.labels << "slot";
        if (momentOf[i] >= 0) {
            const DirectMoment& m = moments[momentOf[i]];
            x.file = rangeOf[i].file;
            x.from = rangeOf[i].from;
            x.to = rangeOf[i].to;
            x.score = m.score;
            x.reason = m.reason;
            for (const QString& l : m.labels)
                if (!x.labels.contains(l)) x.labels << l;
        } else if (p["shot"].isNull()) {
            s.notes << Direction::makeNote("no_shot", {{"slot", x.slot}});
            continue;
        } else {
            // No moment left: the index's shot for the field (sharp, well exposed …), as long as the field
            ++fromShots;
            x.shot = p["shot"].toInt();
            x.file = absolute(p["file"].toString());
            x.from = p["from_s"].toDouble();
            const double want = slotSeconds(slotInfo.value(x.slot), p["to_s"].toDouble() - x.from);
            x.to = want > 0 ? std::min(p["to_s"].toDouble(), x.from + want) : p["to_s"].toDouble();
            x.score = p["score"].toDouble();
            x.reason = {"slot_match", {{"slot", x.slot}}};
            if (const QString why = p["reason"]["text"].toString(); !why.isEmpty()) x.reason.values["why"] = why;
        }
        x.n = int(s.suggestions.size()) + 1;
        s.suggestions << x;
        s.order << x.n;
    }
    if (!moments.isEmpty() && fromShots > 0)
        s.notes << Direction::makeNote("few_slot_moments", {{"n", fromShots}});
    // Text and music fields are not chosen here: they keep the template's content unless apply gives values
    QStringList texts;
    for (const QJsonValue& u : r["unfilled"].toArray()) {
        const QJsonObject slot = slotInfo.value(u.toString());
        if (slot["kind"].toString() == QLatin1String("clip")) continue;
        const QString now = slot["instances"][0]["text"].toString();
        texts << (now.isEmpty() ? u.toString() : QString("%1 (\"%2\")").arg(u.toString(), now));
    }
    if (!texts.isEmpty()) s.notes << Direction::makeNote("fields_kept", {{"fields", texts.join(", ")}});
    for (const QJsonValue& n : r["notes"].toArray()) s.notes << Direction::Note{"template", n.toString()};
}

QJsonObject applyTemplate(const Direction::Session& s, const QVector<const Direction::Suggestion*>& list,
                          const QJsonObject& args, Context& ctx)
{
    // The text and music fields the caller fills (apply values {slot: value}), then the chosen clips
    QJsonObject values = args["apply"].toObject()["values"].toObject();
    for (const Direction::Suggestion* x : list)
        values[x->slot] = QJsonObject{{"file", x->file}, {"from", seconds3(x->from)}, {"to", seconds3(x->to)}};
    const QString tmpl = s.options["template"].toString().trimmed();
    QJsonObject t{{"values", values}};
    if (args["dry_run"].toBool()) t["dry_run"] = true;
    QString project = args["project"].toString();
    if (!project.isEmpty() && !Live::isAppProject(project)) project = absolute(project);
    if (!tmpl.isEmpty()) {
        // A content template: a new project of it (the app opens nothing by itself; the answer names the file)
        if (project.isEmpty() || QFileInfo::exists(project) || Live::isAppProject(project)) {
            // In project_dir (the app: its own folder for projects), else the folder for projects (Settings >
            // Project); the app's page without an own folder: next to the first clip
            const QFileInfo first(list.isEmpty() ? QString() : list.first()->file);
            QString dir = args["project_dir"].toString().trimmed();
            // A work folder (`mcp --root`): the project goes into it, everything else would be OUTSIDE_ROOT
            dir = !dir.isEmpty()      ? absolute(dir)
                  : Live::host()      ? first.absolutePath()
                  : Sandbox::active() ? Sandbox::root()
                                      : AppPaths::projectFolder();
            if (!args["dry_run"].toBool() && !QDir().mkpath(dir)) fail("SAVE_FAILED", "cannot make the folder " + dir);
            QString base = QDir(dir).filePath(QFileInfo(tmpl).completeBaseName());
            project = base + "." + QString(ProjectFile::Extension);
            for (int i = 2; QFileInfo::exists(project); ++i) project = QString("%1-%2.%3").arg(base).arg(i).arg(ProjectFile::Extension);
        }
        t["fill"] = tmpl;
        t["out_project"] = absolute(project);
    } else {
        if (project.isEmpty()) project = s.project;
        if (project.isEmpty() && Live::host()) project = Live::AppProject;
        if (project.isEmpty()) fail("NO_MATERIAL", "template without a template id fills the slots of a project: give project");
        t["project"] = project; // in place: one undo step in the app
    }
    const QJsonObject r = runSub("templates", t, ctx);
    QJsonArray filled;
    for (const Direction::Suggestion* x : list)
        filled << QJsonObject{{"slot", x->slot}, {"file", x->file}, {"from_s", std::round(x->from * 100) / 100},
                             {"to_s", std::round(x->to * 100) / 100}};
    QJsonObject out{{"project", project}, {"slots", filled}, {"done", QJsonArray{QString("filled %1 field%2").arg(list.size()).arg(list.size() == 1 ? "" : "s")}},
                    {"session", s.id}, {"result", r}};
    if (!tmpl.isEmpty()) out["new_project"] = true;
    out["next"] = tmpl.isEmpty() ? QString("look at it, then `render`") : QString("open %1 (app open) or `render` it").arg(project);
    return out;
}

// ---------- script ----------

void suggestScript(Direction::Session& s, const QStringList& files, Context& ctx)
{
    // Files alone (no project): the takes are found in a scratch project of them; applying makes the real one (in the
    // folder for projects, like the other recipes)
    // A work folder (`mcp --root`): the scratch folder inside it (a system one would be OUTSIDE_ROOT), not hidden (the
    // root refuses hidden ones), removed again with this suggestion
    QTemporaryDir scratch(Sandbox::active() ? QDir(Sandbox::root()).filePath("schneidi-script-XXXXXX") : QString());
    QString project;
    if (s.project.isEmpty() && !files.isEmpty() && !Live::host() && !Live::appCopy()) {
        if (!scratch.isValid()) fail("SAVE_FAILED", "cannot make a temporary folder");
        project = scratch.filePath("script." + QString(ProjectFile::Extension));
        runSub("new", {{"project", project}, {"media", QJsonArray::fromStringList(files)}}, ctx);
    } else {
        project = projectOf(s);
    }
    QJsonObject op{{"op", "rough_cut"}};
    for (const char* k : {"script", "script_file", "takes", "language"})
        if (s.options.contains(k)) op[k] = s.options[k];
    if (!op.contains("script") && !op.contains("script_file"))
        fail("BAD_ARGUMENT", "recipe script needs options.script (the text) or options.script_file");
    if (!files.isEmpty()) op["media"] = QJsonArray::fromStringList(files);
    QJsonObject e{{"project", project}, {"ops", QJsonArray{op}}, {"dry_run", true}};
    // Files without a transcript: whisper here (a background job), once per file and language - what it made is kept
    // (AnalysisCache "speech-words") for the next suggestion and goes into the project with the apply
    if (!files.isEmpty()) {
        QJsonObject keyOptions{{"language", s.options["language"].toString()}};
        if (const QString id = Vocabulary::cacheId(Vocabulary::stored()); !id.isEmpty()) keyOptions["vocabulary"] = id;
        QJsonObject prepared;
        QStringList missing;
        for (const QString& f : files) {
            if (const auto hit = AnalysisCache::get(AnalysisCache::key("speech-words", f, keyOptions, 1)))
                prepared[f] = *hit;
            else
                missing << f;
        }
        if (!missing.isEmpty()) {
            QJsonObject only = op;
            only["media"] = QJsonArray::fromStringList(missing);
            const QJsonObject made = prepareTalk({{"project", project}, {"ops", QJsonArray{only}}}, ctx);
            const QJsonObject t = made["ops"].toArray().first().toObject()["_transcripts"].toObject();
            for (auto it = t.begin(); it != t.end(); ++it) {
                prepared[it.key()] = it.value();
                AnalysisCache::put(AnalysisCache::key("speech-words", it.key(), keyOptions, 1), it.value().toObject());
            }
        }
        failIfCanceled();
        if (!prepared.isEmpty()) {
            op["_transcripts"] = prepared;
            e["ops"] = QJsonArray{op};
            s.options["_transcripts"] = prepared;
        }
    }
    const QJsonObject r = runSub("edit", e, ctx);
    const QJsonObject note = noteObject(r["notes"].toArray(), "rough_cut");
    QStringList used;
    const QJsonArray sentences = note["sentences"].toArray();
    for (int i = 0; i < sentences.size(); ++i) {
        const QJsonObject sentence = sentences[i].toObject();
        const auto add = [&](const QJsonObject& take, bool on) {
            Direction::Suggestion x;
            x.n = int(s.suggestions.size()) + 1;
            x.kind = "take";
            x.sentence = i + 1;
            x.file = absolute(take["file"].toString());
            // What rough_cut would place (just before the first word to the end of the last one)
            x.from = take["cut_from_s"].toDouble(take["from_s"].toDouble());
            x.to = take["cut_to_s"].toDouble(take["to_s"].toDouble());
            x.score = take["coverage"].toDouble();
            x.on = on;
            x.reason = {on ? "take" : "alt_take", {{"sentence", i + 1}}};
            x.labels << (take["good"].toBool() ? "good" : "take");
            if (!used.contains(x.file)) used << x.file;
            s.suggestions << x;
            if (on) s.order << x.n;
        };
        if (sentence["take"].isObject()) add(sentence["take"].toObject(), true);
        for (const QJsonValue& alt : sentence["alternatives"].toArray()) add(alt.toObject(), false);
    }
    for (const QJsonValue& m : note["missing"].toArray())
        s.notes << Direction::makeNote("not_said", {{"text", m.toString()}});
    for (const QString& f : files.isEmpty() ? used : files) s.material << materialOf(absolute(f));
}

} // namespace Cli::detail
