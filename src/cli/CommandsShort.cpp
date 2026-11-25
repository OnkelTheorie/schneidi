// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli `short`: from recordings (or a timeline) to a finished vertical short in one call. Only strings the
// other commands together (probe, new, `direct` recipe short + apply, transcribe, check + its fix, render), each
// through execute() like a caller would: live in the app with "@app"; the moments and the cut are `direct`'s
// (cli/CommandsDirect.cpp, core/ShortPick).
#include "cli/AnalysisDetail.h"
#include "cli/CommandsDetail.h"
#include "cli/DirectDetail.h"
#include "cli/Live.h"

#include "core/Captions.h"
#include "core/GameHud.h"
#include "core/I18n.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/SfxPlace.h"
#include "core/SfxSynth.h"
#include "core/ShortPick.h"
#include "core/StreamPresets.h"
#include "core/TranscriptCut.h"
#include "core/Vocabulary.h"
#include "engine/AudioAnalysis.h"
#include "engine/Extensions.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
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

QString checkModelFor(const QString& mainModel)
{
    const QString name = QFileInfo(mainModel).fileName();
    const QString large = Extensions::whisperModelPath(Extensions::qualityModel("accurate"));
    const QString turbo = Extensions::whisperModelPath(Extensions::qualityModel("fast"));
    const auto other = [&](const QString& path) { return !path.isEmpty() && QFileInfo(path).fileName() != name; };
    if (name.contains(QLatin1String("turbo"))) return other(large) ? large : QString();
    if (other(turbo)) return turbo;
    if (other(large)) return large;
    for (const QString& id : Extensions::installedWhisperModels())
        if (const QString path = Extensions::whisperModelPath(id); other(path)) return path;
    return {};
}

void checkCaptionsCheck(const QJsonValue& v, const char* name)
{
    if (v.isUndefined() || v.isNull() || v.isBool()) return;
    const QString t = v.toString().trimmed().toLower();
    if (!v.isString() || !(t.isEmpty() || t == QLatin1String("auto") || t == QLatin1String("on") || t == QLatin1String("off")))
        fail("BAD_ARGUMENT", QString("%1 must be auto, on or off").arg(QLatin1String(name)));
}

bool fastCheckModel(const QString& model)
{
    const QString name = QFileInfo(model).fileName();
    return !name.contains(QLatin1String("large")) || name.contains(QLatin1String("turbo"));
}

bool cheapCheck(bool fastModel, double seconds)
{
    return seconds <= 0 || !Extensions::whisperGpuBackend().isEmpty() || (fastModel && seconds < 60);
}

QJsonArray voiceCaptionWords(const QString& project, const QString& timeline, const QString& language, Context& ctx,
                             QStringList* voices, const QString& game)
{
    VoiceCaptions v;
    v.language = language;
    v.game = game;
    const QJsonArray out = voiceCaptionWords(project, timeline, ctx, &v);
    *voices = v.voices;
    return out;
}

QJsonArray voiceCaptionWords(const QString& project, const QString& timeline, Context& ctx, VoiceCaptions* v)
{
    Session view(project);
    if (!timeline.isEmpty() && timeline != QLatin1String("@current")) view.selectTimeline(timeline);
    const Timeline& tl = view.project.timeline();
    QVector<MediaInfo> media = view.project.media();
    const auto info = [&media](const QString& path) -> MediaInfo* {
        for (MediaInfo& m : media)
            if (m.path == path) return &m;
        return nullptr;
    };
    // The voice tracks: audible audio tracks whose clips all play a voice stream (microphone, voice chat) of a
    // recording with several streams
    const bool solo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    QVector<int> tracks;
    for (int i = 0; i < tl.audio.size(); ++i) {
        const Track& t = tl.audio[i];
        if (t.clips.isEmpty() || t.muted || (solo && !t.solo)) continue;
        bool all = true;
        for (const Clip& c : t.clips) {
            const MediaInfo* m = c.mediaPath.isEmpty() ? nullptr : info(c.mediaPath);
            all = all && m && m->audioStreamCount() > 1 && voiceStream(m->audioStreamName(c.audioStream));
        }
        if (all) tracks << i;
    }
    if (tracks.isEmpty()) return {};
    const QStringList terms = Vocabulary::gameTerms(v->game);
    const double fps = view.project.frameRate();
    // The (file, stream)s of the voice tracks, each with the parts of it the timeline plays (a 45 s short of a 30 min
    // recording hears those, not the whole file)
    struct Voice {
        QString file;
        int stream = 0;
        QVector<QPair<qint64, qint64>> parts;
        QString model; // the model that made its transcript (file name)
        QString checkModel;
    };
    QVector<Voice> streams;
    for (const int i : tracks)
        for (const Clip& c : tl.audio[i].clips)
            if (std::none_of(streams.begin(), streams.end(), [&](const Voice& x) { return x.file == c.mediaPath && x.stream == c.audioStream; }))
                streams << Voice{c.mediaPath, c.audioStream, TranscriptCut::playedRangesMs(tl, fps, -1, c.mediaPath, c.audioStream), {}, {}};
    const auto transcriptOf = [&](const Voice& s) -> MediaTranscript* {
        if (MediaInfo* m = info(s.file))
            for (MediaTranscript& x : m->transcripts)
                if (x.audioStream == s.stream) return &x;
        return nullptr;
    };
    // Each (file, stream) transcribed alone, part by part through the cache; a transcript the project has is kept
    int heard = 0;
    for (Voice& s : streams) {
        if (transcriptOf(s)) {
            s.model = QFileInfo(defaultTranscribeModel()).fileName(); // (which one made it is not known)
            ++heard;
            continue;
        }
        MediaInfo* m = info(s.file);
        if (!m) continue;
        MediaTranscript t;
        t.audioStream = s.stream;
        bool any = false;
        for (const auto& [from, to] : std::as_const(s.parts)) {
            failIfCanceled();
            const std::optional<QJsonObject> one = streamTranscript(s.file, s.stream, v->language, false, ctx, terms, {}, from, to);
            if (!one) continue;
            if (!any) {
                t.language = (*one)["language"].toString();
                s.model = (*one)["model"].toString();
            }
            any = true;
            for (const QJsonValue& w : (*one)["words"].toArray())
                t.words << TranscriptWord{qint64(w[0].toDouble()), qint64(w[1].toDouble()), w[2].toString()};
        }
        if (!any) continue; // whisper could not hear it (not "no speech")
        if (s.model.isEmpty()) s.model = QFileInfo(defaultTranscribeModel()).fileName(); // (an entry from before)
        if (t.language.isEmpty())
            for (const auto& [from, to] : std::as_const(s.parts)) // a part without speech names no language
                if (const auto hit = AnalysisCache::get(streamTranscriptKey(s.file, s.stream, v->language, false, terms, {}, from, to)))
                    if (const QString l = (*hit)["language"].toString(); !l.isEmpty()) t.language = l;
        m->transcripts << t;
        ++heard;
    }
    // No voice stream could be transcribed (whisper failed on each, not "no speech"): the timeline's sound instead
    if (heard == 0) {
        v->voices.clear();
        return {};
    }
    // The check: the same parts heard by another whisper model than the one that made the transcript; a phrase stays
    // only when both hear the same words (automatic captions nobody proofreads: rather no line than a wrong one)
    QHash<QString, QVector<Captions::Disagreement>> disagreements; // "file\nstream"
    if (v->check != QLatin1String("off")) {
        double seconds = 0; // the sound the check must transcribe (its cached parts not)
        bool fast = true;
        QStringList models;
        for (Voice& s : streams) {
            const MediaTranscript* t = transcriptOf(s);
            if (!t || t->words.isEmpty()) continue;
            s.checkModel = !v->checkModel.isEmpty() ? v->checkModel : checkModelFor(s.model);
            if (s.checkModel.isEmpty()) continue;
            if (const QString name = QFileInfo(s.checkModel).fileName(); !models.contains(name)) models << name;
            fast = fast && fastCheckModel(s.checkModel);
            for (const auto& [from, to] : std::as_const(s.parts))
                if (!AnalysisCache::get(streamTranscriptKey(s.file, s.stream, t->language, false, terms, s.checkModel, from, to)))
                    seconds += (to - from) / 1000.0;
        }
        if (models.isEmpty()) {
            v->skipped = "no_model";
        } else if (v->check == QLatin1String("auto") && v->checkModel.isEmpty() && !cheapCheck(fast, seconds)) {
            v->skipped = "slow";
        } else {
            v->checkedWith = models.join(", ");
            QElapsedTimer clock;
            clock.start();
            for (const Voice& s : std::as_const(streams)) {
                MediaTranscript* t = transcriptOf(s);
                if (!t || t->words.isEmpty() || s.checkModel.isEmpty()) continue;
                QVector<TranscriptWord> other;
                bool all = true;
                for (const auto& [from, to] : std::as_const(s.parts)) {
                    failIfCanceled();
                    const std::optional<QJsonObject> two = streamTranscript(s.file, s.stream, t->language, false, ctx, terms,
                                                                            s.checkModel, from, to);
                    if (!two) {
                        all = false;
                        break;
                    }
                    for (const QJsonValue& w : (*two)["words"].toArray())
                        other << TranscriptWord{qint64(w[0].toDouble()), qint64(w[1].toDouble()), w[2].toString()};
                }
                if (!all) continue; // the check could not run: the words stay as heard
                QVector<Captions::Disagreement> left;
                t->words = Captions::keepAgreed(t->words, other, 700, &left);
                disagreements[s.file + '\n' + QString::number(s.stream)] = left;
            }
            v->checkSeconds = clock.elapsed() / 1000.0;
        }
    }
    // Per track on the timeline (trims and speed followed), each phrase moved onto its stream's sound (whisper's
    // times are loose; a voice stream is quiet between words), then one list in time order
    const int pause = int(std::lround(0.7 * fps));
    QVector<QVector<TranscriptCut::Word>> perTrack;
    for (const int i : tracks) {
        failIfCanceled();
        QVector<TranscriptCut::Word> words = TranscriptCut::timelineWords(tl, media, fps, i, true);
        if (!words.isEmpty())
            if (const auto peaks = AudioAnalysis::tracksFramePeaks(view.project.timelineFormat(), tl, {i}))
                Captions::snapToSound(words, *peaks, fps, pause);
        perTrack << words;
        QString name = tl.audio[i].name;
        if (name.isEmpty())
            for (const Clip& c : tl.audio[i].clips)
                if (const MediaInfo* m = info(c.mediaPath)) name = m->audioStreamName(c.audioStream);
        v->voices << (name.isEmpty() ? QString("A%1").arg(i + 1) : name);
    }
    // The lines left out that lie in the timeline: each phrase as one "word" through the same mapping
    if (!disagreements.isEmpty()) {
        QVector<MediaInfo> marks = media;
        QVector<Captions::Disagreement> all;
        for (MediaInfo& m : marks)
            for (MediaTranscript& t : m.transcripts) {
                t.words.clear();
                for (const Captions::Disagreement& d : disagreements.value(m.path + '\n' + QString::number(t.audioStream))) {
                    t.words << TranscriptWord{d.from, d.to, QString("#%1").arg(all.size())};
                    all << d;
                }
            }
        for (int k = 0; k < tracks.size(); ++k)
            for (const TranscriptCut::Word& w : TranscriptCut::timelineWords(tl, marks, fps, tracks[k], true)) {
                const Captions::Disagreement& d = all[w.text.mid(1).toInt()];
                v->leftOut << QJsonObject{{"from_s", std::round(w.from / fps * 100) / 100},
                                          {"to_s", std::round(w.to / fps * 100) / 100},
                                          {"speaker", v->voices.value(k)}, {"heard", d.heard}, {"other", d.other}};
            }
    }
    QJsonArray out;
    for (const TranscriptCut::Word& w : Captions::mergeSpeakers(perTrack))
        out << QJsonObject{{"from", w.from}, {"to", std::max(w.from + 1, w.spokenTo)}, {"text", w.text}, {"speaker", w.track}};
    return out;
}

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
            // Each voice track transcribed on its own (a quiet voice chat partner is lost in a mix with the
            // microphone), else the timeline's sound as `transcribe` hears it
            const QString termsOf = captionTermsGame(f);
            VoiceCaptions vc;
            vc.language = f.language;
            vc.game = termsOf;
            vc.check = f.captionsCheck;
            QJsonArray words = voiceCaptionWords(project, timeline, ctx, &vc);
            const QStringList voices = vc.voices;
            int segments = 0;
            if (voices.isEmpty()) {
                QJsonObject t{{"source", project}, {"timeline", timeline}, {"words", true}};
                if (!f.language.isEmpty()) t["language"] = f.language;
                if (const QStringList terms = Vocabulary::gameTerms(termsOf); !terms.isEmpty())
                    t["vocabulary"] = QJsonArray::fromStringList(terms);
                try {
                    const QJsonObject tr = run("transcribe", t);
                    segments = int(tr["segments"].toArray().size());
                    // Only words that start inside the timeline (as transcribe apply did)
                    const int end = run("info", {{"project", project}, {"timeline", timeline}, {"summary", true}})["timeline"]
                                        .toObject()["length"].toInt();
                    for (const QJsonValue& w : tr["words"].toArray())
                        if (end <= 0 || w["from"].toInt() < end) words << w;
                    // The check here too: the timeline's sound heard by the other model, the same rule
                    if (vc.check != QLatin1String("off") && !words.isEmpty()) {
                        const double fps = tr["fps"].toDouble(25);
                        const QString checkModel = checkModelFor(tr["model"].toString());
                        const double seconds = (end > 0 ? end : words.last()["to"].toInt()) / std::max(1.0, fps);
                        if (checkModel.isEmpty()) {
                            vc.skipped = "no_model";
                        } else if (vc.check == QLatin1String("auto") && !cheapCheck(fastCheckModel(checkModel), seconds)) {
                            vc.skipped = "slow";
                        } else {
                            QElapsedTimer clock;
                            clock.start();
                            QJsonObject t2 = t;
                            t2["model"] = checkModel;
                            if (!tr["language"].toString().isEmpty()) t2["language"] = tr["language"];
                            std::optional<QJsonObject> two;
                            try {
                                two = run("transcribe", t2);
                            } catch (const Error& e) {
                                if (e.code == QLatin1String("CANCELED")) throw;
                                if (e.code == QLatin1String("NO_SPEECH")) two = QJsonObject{};
                                // else the check could not run: the words stay as heard
                            }
                            if (two) {
                                const auto ms = [fps](const QJsonValue& v) { return qint64(std::llround(v.toDouble() / fps * 1000)); };
                                QVector<TranscriptWord> heardWords, other;
                                for (const QJsonValue& w : std::as_const(words))
                                    heardWords << TranscriptWord{ms(w["from"]), ms(w["to"]), w["text"].toString()};
                                for (const QJsonValue& w : (*two)["words"].toArray())
                                    other << TranscriptWord{ms(w["from"]), ms(w["to"]), w["text"].toString()};
                                QVector<Captions::Disagreement> left;
                                const QVector<TranscriptWord> kept = Captions::keepAgreed(heardWords, other, 700, &left);
                                // The kept words are a part of the heard ones, in their order
                                QJsonArray agreed;
                                qsizetype k = 0;
                                for (qsizetype i = 0; i < heardWords.size() && k < kept.size(); ++i)
                                    if (heardWords[i].from == kept[k].from && heardWords[i].text == kept[k].text) {
                                        agreed << words[i];
                                        ++k;
                                    }
                                words = agreed;
                                for (const Captions::Disagreement& d : std::as_const(left))
                                    vc.leftOut << QJsonObject{{"from_s", std::round(d.from / 10.0) / 100},
                                                              {"to_s", std::round(d.to / 10.0) / 100},
                                                              {"heard", d.heard}, {"other", d.other}};
                                vc.checkedWith = QFileInfo(checkModel).fileName();
                                vc.checkSeconds = clock.elapsed() / 1000.0;
                            }
                        }
                    }
                } catch (const Error& e) {
                    if (e.code != QLatin1String("NO_SPEECH")) throw;
                }
            }
            if (!vc.checkedWith.isEmpty()) {
                *done << QString("captions checked with a second model (%1, %2 s): %3 line(s) left out, the two models "
                                 "heard them differently")
                             .arg(vc.checkedWith)
                             .arg(std::round(vc.checkSeconds * 10) / 10)
                             .arg(vc.leftOut.size());
                if (!vc.leftOut.isEmpty()) note("captions_left_out");
                if (f.leftOut) *f.leftOut = vc.leftOut;
            } else if (vc.skipped == QLatin1String("no_model")) {
                note("captions_check_no_model");
            } else if (vc.skipped == QLatin1String("slow")) {
                note("captions_check_slow");
            }
            if (words.isEmpty() && !vc.leftOut.isEmpty()) {
                note("captions_all_left_out"); // speech was there, the two models agreed on none of it
            } else if (words.isEmpty()) {
                note("no_captions_speech");
            } else {
                QJsonObject op{{"op", "captions"}, {"style", f.captions}, {"words", words}};
                if (voices.size() > 1) op["speakers"] = QJsonArray::fromStringList(voices);
                ops << op;
                *done << (voices.isEmpty() ? QString("%1 captions (%2 segments)").arg(f.captions).arg(segments)
                                           : QString("%1 captions (%2 words) of %3, each voice transcribed on its own")
                                                 .arg(f.captions)
                                                 .arg(words.size())
                                                 .arg(voices.join(", ")));
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
    checkCaptionsCheck(a["captions_check"], "captions_check");
    const ShortPlatform* platform = shortPlatform(a.contains("for") ? a["for"].toString() : QString("shorts"));
    if (!platform) fail("BAD_ARGUMENT", "for must be shorts, reels or tiktok (the platform)");
    if (platform->maxSeconds < pick.maxS) pick.maxS = platform->maxSeconds;
    pick.minS = std::min(pick.minS, pick.maxS);
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
    // Not named: looked for in the picture (a clip tool's "Desktop_clip_…" of CS2), said in done
    bool gameSeen = false;
    if (!game && !noGame && !files.isEmpty()) {
        Context sub;
        sub.cancel = ctx.cancel;
        if (const QString seen = gameInPicture(files, sub); !seen.isEmpty()) {
            game = GameHud::find(seen);
            gameSeen = game != nullptr;
        }
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

    QJsonArray done, notes, noteKeys; // noteKeys: the fixed keys of the recipe's notes (Direction::noteText)
    if (gameSeen) done << QString("game: %1 (recognised in the picture)").arg(game->id);
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
        QVector<int> candidates(files.size(), 0); // highlight candidates per recording (voice and clip end too)
        QVector<int> real(files.size(), 0);       // ... only kills / loud moments
        int voiceCount = 0, endCount = 0;
        for (const DirectMoment& m : found.highlights) {
            ++candidates[m.file];
            voiceCount += m.source == QLatin1String("voice");
            endCount += m.source == QLatin1String("end");
            real[m.file] += m.source == QLatin1String("highlights");
        }
        const int count = int(found.highlights.size()) - voiceCount - endCount;
        const int withSome = int(std::count_if(real.begin(), real.end(), [](int n) { return n > 0; }));
        done << QString("highlights: %1 candidate%2%3 in %4 of %5 recording%6")
                    .arg(count)
                    .arg(count == 1 ? "" : "s")
                    .arg(game ? QString(" (kill feed of %1)").arg(game->id) : QString(" (loud moments)"))
                    .arg(withSome)
                    .arg(files.size())
                    .arg(files.size() == 1 ? "" : "s");
        if (voiceCount > 0)
            done << QString("voices: %1 moment%2 of laughter or loud voices in a voice stream")
                        .arg(voiceCount)
                        .arg(voiceCount == 1 ? "" : "s");
        if (endCount > 0)
            done << QString("clip ends: %1 replay clip%2 without a kill offer%3 its end (saved right after the moment)")
                        .arg(endCount)
                        .arg(endCount == 1 ? "" : "s")
                        .arg(endCount == 1 ? "s" : "");
        if (count + voiceCount + endCount == 0) {
            if (found.extra.isEmpty())
                notes << "no highlight found: the short is the end of the recording (where a clip tool saves the moment)";
            else
                notes << "no highlight found: the short is made of sound events and picture changes";
        }
        // The recipe's notes a short's caller needs (key + text): shorter than min_s, and why
        for (const Direction::Note& n : session.notes)
            if (n.key == QLatin1String("short_shorter")) {
                notes << n.text;
                noteKeys << n.key;
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
    ShortFinish finish{captions, sfx, game ? game->id : QString(), a["language"].toString(),
                       Vocabulary::useGameTerms(a["game_terms"]), Vocabulary::captionsCheck(a["captions_check"])};
    QJsonArray leftOut;
    finish.leftOut = &leftOut;
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

    // 9. A contact sheet of the video to look at (8 tiles), next to it - never replacing a file (numbered)
    QString sheet;
    if (a["sheet"].toBool(true)) {
        const QFileInfo o(out);
        sheet = freeName(o.absolutePath(), o.completeBaseName() + "-sheet", ".jpg");
        try {
            run("frames", {{"source", out}, {"count", 8}, {"sheet", true}, {"columns", 4}, {"tile_width", 270}, {"out", sheet}},
                step(1, 1));
            done << "sheet " + sheet;
        } catch (const Error& e) {
            if (e.code == QLatin1String("CANCELED")) throw;
            notes << "no contact sheet: " + e.message;
            sheet.clear();
        }
    }

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
    if (!leftOut.isEmpty()) o["captions_left_out"] = leftOut; // lines two speech models heard differently
    if (rendered.contains("encoder")) o["encoder"] = rendered["encoder"];
    for (const char* k : {"threads", "threads_reason"}) // how many render threads, fewer when little RAM was free
        if (rendered.contains(k)) o[k] = rendered[k];
    if (rendered.contains("encoder_hint")) notes << rendered["encoder_hint"];
    if (!sheet.isEmpty()) o["sheet"] = sheet;
    if (!notes.isEmpty()) o["notes"] = notes;
    if (!noteKeys.isEmpty()) o["note_keys"] = noteKeys;
    o["next"] = sheet.isEmpty() ? QString("look at the result: `frames` of the out file (sheet) and `check` on it")
                                : QString("look at the sheet (8 frames of the video) - more with `frames` of the out file");
    return o;
}

} // namespace Cli::detail
