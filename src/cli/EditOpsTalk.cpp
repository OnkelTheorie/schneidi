// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli edit operations for talk (interviews, vlogs, podcasts): J/L cuts (the sound edge of a cut alone),
// room tone in the gaps of a dialogue track, a rough cut from a script and the best takes (core/ScriptAlign)
#include "cli/CommandsDetail.h"

#include "cli/Commands.h"
#include "core/IndexSearch.h"
#include "core/JumpCut.h"
#include "core/Keyframes.h"
#include "core/RoomTone.h"
#include "core/ScriptAlign.h"
#include "core/TimelineOps.h"
#include "core/Transcript.h"
#include "core/TranscriptCut.h"
#include "engine/AudioAnalysis.h"
#include "engine/MediaIndex.h"
#include "engine/ReframeAnalysis.h"
#include "engine/Extensions.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Cli::detail {

namespace {

// ---------- jl_cut ----------

// A signed time: frames, or "-0.5s" / "+12" / a timecode with a sign
int signedTime(Session& s, const QJsonValue& v, const QString& what)
{
    if (v.isDouble()) return int(std::lround(v.toDouble()));
    QString t = v.toString().trimmed();
    const bool minus = t.startsWith('-');
    if (minus || t.startsWith('+')) t = t.mid(1).trimmed();
    if (t.isEmpty()) fail("BAD_ARGUMENT", QString("'%1' is no time (frames, \"-0.5s\", \"+12\")").arg(what));
    const int f = parseTime(t, s.format(), what);
    return minus ? -f : f;
}

QString frameSpan(int frames, const ProjectFormat& f)
{
    return QString("%1 frames (%2 s)").arg(frames).arg(seconds(frames, f), 0, 'f', 2);
}

// The cut a jl_cut op means: (left clip, right clip) of a picture cut whose clips have linked sound
QPair<int, int> jlCutAt(Session& s, const QJsonObject& op)
{
    using namespace TimelineOps;
    const Timeline& tl = s.project.timeline();
    if (op.contains("clip")) {
        const Clip& given = clipOf(s, op);
        TrackRef ref;
        findClip(tl, given.id, &ref);
        const Clip* pic = &given;
        if (ref.kind == TrackKind::Audio) { // its picture stands for it
            pic = nullptr;
            for (int i = 0; i < tl.video.size() && !pic && given.linkId; ++i)
                for (const Clip& c : tl.video[i].clips)
                    if (c.linkId == given.linkId) {
                        pic = &c;
                        ref = {TrackKind::Video, i};
                        break;
                    }
            if (!pic)
                fail("BAD_ARGUMENT", QString("clip %1 is sound without a linked picture: a J/L cut moves the sound "
                                             "edge of a picture cut (a sound cut alone: trim mode:'roll')")
                                         .arg(given.id));
        }
        const QString edge = op.value("edge").toString("start");
        if (edge != "start" && edge != "end") fail("BAD_ARGUMENT", "edge must be 'start' or 'end'");
        const bool start = edge == "start";
        const int cut = start ? pic->start : pic->end();
        for (const Clip& o : tl.track(ref).clips)
            if (o.id != pic->id && (start ? o.end() == cut : o.start == cut))
                return start ? qMakePair(o.id, pic->id) : qMakePair(pic->id, o.id);
        fail("FAILED", QString("no clip meets clip %1 at its %2 (%3): a J/L cut needs two clips that touch")
                           .arg(pic->id)
                           .arg(edge, tc(cut, s.format())));
    }
    if (!op.contains("at")) fail("BAD_ARGUMENT", "missing 'at' (time of the cut) or 'clip' (the clip after the cut)");
    const int at = timeOf(s, op, "at");
    QPair<int, int> best{0, 0};
    int bestDist = INT_MAX;
    for (const Track& t : tl.video)
        for (int k = 0; k + 1 < t.clips.size(); ++k) {
            const Clip& a = t.clips[k];
            const Clip& b = t.clips[k + 1];
            if (a.end() != b.start || std::abs(b.start - at) >= bestDist || !s.editor.soundEdge(a.id, b.id)) continue;
            bestDist = std::abs(b.start - at);
            best = {a.id, b.id};
        }
    if (!best.first)
        fail("NOT_FOUND", "no picture cut whose clips both have linked sound that meets on an audio track (link "
                          "picture and sound with op 'link'; a sound cut alone: trim mode:'roll')");
    return best;
}

void opJlCut(Session& s, const QJsonObject& op)
{
    if (!op.contains("audio"))
        fail("BAD_ARGUMENT", "missing 'audio': where the sound cut lies from the picture cut, '-0.5s' = J (the next "
                             "clip's sound starts early), '+0.5s' = L (the previous sound runs on), 0 = back in sync");
    const int offset = signedTime(s, op.value("audio"), "audio");
    const auto [left, right] = jlCutAt(s, op);
    const std::optional<Editor::SoundEdge> e = s.editor.soundEdge(left, right);
    const ProjectFormat& f = s.format();
    if (!e)
        fail("FAILED", QString("the clips %1 and %2 at the cut have no linked sound clips that meet on an unlocked audio "
                               "track (link them with op 'link'; a sound cut alone: trim mode:'roll')")
                           .arg(left)
                           .arg(right));
    if (offset < e->lo || offset > e->hi)
        fail("BAD_ARGUMENT",
             QString("audio %1 needs more material than the clips have: at this cut the sound can start up to %2 early "
                     "(audio:'-%3s') and run on up to %4 (audio:'+%5s')")
                 .arg(offset)
                 .arg(frameSpan(std::max(0, -e->lo), f))
                 .arg(seconds(std::max(0, -e->lo), f), 0, 'f', 2)
                 .arg(frameSpan(std::max(0, e->hi), f))
                 .arg(seconds(std::max(0, e->hi), f), 0, 'f', 2));
    s.editor.setSoundEdge(left, right, offset);
    QStringList tracks;
    for (const auto& p : e->pairs) {
        TrackRef ref;
        if (TimelineOps::findClip(s.project.timeline(), p.first, &ref)) tracks << trackName(ref);
    }
    const QString what = offset < 0   ? QString("J cut: the sound of clip %1 starts %2 before").arg(right).arg(frameSpan(-offset, f))
                         : offset > 0 ? QString("L cut: the sound of clip %1 runs %2 past").arg(left).arg(frameSpan(offset, f))
                                      : QString("straight cut: the sound cuts with");
    s.notes << QString("jl_cut: %1 the picture cut at %2 (sound cut at %3 on %4; picture and links unchanged)")
                   .arg(what, tc(e->cut, f), tc(e->cut + offset, f), tracks.join(", "));
}

// ---------- room_tone ----------

constexpr double kQuietWant = 2.0;  // seconds of quiet the loop is made of
constexpr double kQuietLeast = 1.0; // at least
constexpr double kScanMargin = 30.0; // seconds of the file around the used parts searched for quiet
constexpr double kMaxGainDb = 12.0;

struct TakeQuiet {
    bool found = false;
    int in = 0, length = 0; // source frames
    double floorDb = -200, meanDb = -200;
};

TakeQuiet measureTake(Session& s, const Clip& take, int track)
{
    const MediaInfo* m = s.project.mediaInfo(take.mediaPath);
    const double fps = s.project.frameRate();
    TakeQuiet q;
    if (!m || m->length <= 0 || fps <= 0) return q;
    // The parts of the file this track plays, and some of it around them (the same recording)
    int lo = INT_MAX, hi = INT_MIN;
    for (const Clip& c : s.project.timeline().audio[track].clips)
        if (c.mediaPath == take.mediaPath && c.audioStream == take.audioStream && !c.isRetimed()) {
            lo = std::min(lo, c.in);
            hi = std::max(hi, c.out + 1);
        }
    const int margin = int(std::lround(kScanMargin * fps));
    lo = std::max(0, lo - margin);
    hi = std::min(m->length, hi + margin);
    if (hi <= lo) return q;
    const auto levels = AudioAnalysis::fileBlockLevels(take.mediaPath, take.audioStream, lo / fps, hi / fps,
                                                       RoomTone::kBlockSeconds);
    if (!levels) return q;
    const RoomTone::Quiet quiet = RoomTone::findQuiet(*levels, int(std::lround(kQuietWant / RoomTone::kBlockSeconds)),
                                                      int(std::lround(kQuietLeast / RoomTone::kBlockSeconds)));
    q.floorDb = quiet.floorDb;
    if (!quiet.found()) return q;
    // Whole frames inside the stretch
    q.in = lo + int(std::ceil(quiet.from * RoomTone::kBlockSeconds * fps));
    const int end = lo + int(std::floor(quiet.to * RoomTone::kBlockSeconds * fps));
    q.length = std::min(end, m->length) - q.in;
    q.meanDb = quiet.meanDb;
    q.found = q.length >= 2;
    return q;
}

bool roomToneSource(const Session& s, const Clip& c)
{
    const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
    return c.kind == ClipKind::Media && !c.isRetimed() && m && m->hasAudio && !m->isImage;
}

void opRoomTone(Session& s, const QJsonObject& op)
{
    if (!op.contains("track")) fail("BAD_ARGUMENT", "missing 'track': the dialogue track (\"A1\")");
    TrackKind kind = TrackKind::Audio;
    const int track = parseTrack(op.value("track"), &kind);
    const Timeline& tl = s.project.timeline();
    if (kind != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
    if (track < 0 || track >= tl.audio.size()) fail("NOT_FOUND", QString("there is no A%1").arg(track + 1));
    if (tl.audio[track].locked) fail("FAILED", QString("A%1 is locked").arg(track + 1));
    const ProjectFormat& f = s.format();
    const int from = op.contains("from") ? timeOf(s, op, "from") : 0;
    const int to = op.contains("to") ? timeOf(s, op, "to") : INT_MAX;
    if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");

    // The gaps between clips of the track inside [from, to), each with the take it continues
    struct Gap {
        int from, to;
        Clip take;
    };
    QVector<Gap> gaps;
    int skipped = 0;
    const auto& clips = tl.audio[track].clips; // sorted by start
    for (int k = 0; k + 1 < clips.size(); ++k) {
        const int a = std::max(clips[k].end(), from), b = std::min(clips[k + 1].start, to);
        if (b <= a) continue;
        if (roomToneSource(s, clips[k])) gaps << Gap{a, b, clips[k]};
        else if (roomToneSource(s, clips[k + 1])) gaps << Gap{a, b, clips[k + 1]};
        else ++skipped;
    }
    if (gaps.isEmpty()) {
        s.notes << QString("room_tone: no gaps between recordings on A%1%2").arg(track + 1).arg(
                       op.contains("from") || op.contains("to") ? " in that range" : "");
        return;
    }
    // One measurement per recording
    QHash<QString, TakeQuiet> quiet;
    QVector<RoomTone::Fill> fills;
    QHash<QString, QPair<int, int>> perTake; // key -> (gaps, frames)
    QStringList keys;
    const int seam = RoomTone::seamFrames(s.project.frameRate());
    for (const Gap& g : gaps) {
        const QString key = g.take.mediaPath + '#' + QString::number(g.take.audioStream);
        if (!quiet.contains(key)) {
            quiet.insert(key, measureTake(s, g.take, track));
            keys << key;
        }
        const TakeQuiet& q = quiet[key];
        if (!q.found) continue;
        RoomTone::Fill fill;
        fill.take = g.take;
        fill.from = g.from;
        fill.to = g.to;
        fill.quietIn = q.in;
        fill.quietLength = q.length;
        fill.gainDb = std::clamp(q.floorDb - q.meanDb, -kMaxGainDb, kMaxGainDb);
        fill.seam = seam;
        fills << fill;
        perTake[key].first += 1;
        perTake[key].second += g.to - g.from;
    }
    const QVector<int> ids = s.editor.fillRoomTone(track, fills);
    if (!fills.isEmpty() && ids.isEmpty()) fail("FAILED", QString("could not fill A%1 (locked?)").arg(track + 1));
    for (const QString& key : keys) {
        const TakeQuiet& q = quiet[key];
        const QString file = QFileInfo(key.section('#', 0, -2)).fileName();
        if (!q.found) {
            s.notes << QString("room_tone: %1 has no steady quiet stretch of %2 s (all words, or digital silence): "
                               "its gaps stay empty")
                           .arg(file)
                           .arg(kQuietLeast, 0, 'f', 1);
            continue;
        }
        const double fps = s.project.frameRate();
        s.notes << QString("room_tone: A%1, %2 gap(s) (%3 s) filled with a loop of %4 %5-%6 s (the take's floor "
                           "%7 dBFS, loop %8 dBFS, %9 dB to match)")
                       .arg(track + 1)
                       .arg(perTake[key].first)
                       .arg(seconds(perTake[key].second, f), 0, 'f', 2)
                       .arg(file)
                       .arg(q.in / fps, 0, 'f', 2)
                       .arg((q.in + q.length) / fps, 0, 'f', 2)
                       .arg(q.floorDb, 0, 'f', 1)
                       .arg(q.meanDb, 0, 'f', 1)
                       .arg(std::clamp(q.floorDb - q.meanDb, -kMaxGainDb, kMaxGainDb), 0, 'f', 1);
    }
    if (skipped)
        s.notes << QString("room_tone: %1 gap(s) between clips without recorded sound (titles, music changed in "
                           "speed) stay empty")
                       .arg(skipped);
    if (!ids.isEmpty()) {
        QStringList list;
        for (int id : ids) list << QString::number(id);
        s.notes << QString("room_tone: new clips %1").arg(list.join(", "));
    }
}

// ---------- rough_cut, best_takes ----------

double round2(double v) { return std::round(v * 100) / 100; }

// The words of a file's transcript (the project's, of its first transcribed stream), seconds
QVector<ScriptAlign::Word> wordsOf(const MediaTranscript& t)
{
    QVector<ScriptAlign::Word> out;
    // Up to where the word is spoken (TranscriptWord::to reaches up to the next word: pauses inside a take showed)
    for (const TranscriptWord& w : t.words) out << ScriptAlign::Word{w.text, w.from / 1000.0, Transcript::spokenEnd(w) / 1000.0};
    return out;
}

// whisper on a file in this process (schneidi-cli; never in the app): the words with their spoken end, ms
QVector<TranscriptWord> transcribeFile(const QString& file, const QString& language, QString* lang)
{
    QJsonObject a{{"source", file}, {"words", true}};
    if (!language.isEmpty()) a["language"] = language;
    Context ctx;
    const QJsonObject r = execute(*find("transcribe"), a, ctx);
    const double fps = r["fps"].toDouble(25);
    QVector<TranscriptWord> words;
    for (const QJsonValue& v : r["words"].toArray())
        words << TranscriptWord{qint64(std::lround(v["from"].toDouble() / fps * 1000)),
                                qint64(std::lround(v["to"].toDouble() / fps * 1000)), v["text"].toString()};
    // As every transcript of the project: a word's end reaches up to the next word (TranscriptWord)
    for (int i = 0; i + 1 < words.size(); ++i) words[i].to = std::max(words[i].to, words[i + 1].from);
    if (lang) *lang = r["language"].toString();
    return words;
}

bool canTranscribe() { return !Extensions::whisperProgram().isEmpty() && !Extensions::installedWhisperModels().isEmpty(); }

QStringList roughCutMedia(Session& s, const QJsonObject& op)
{
    QStringList files;
    const QJsonValue given = op.value("media");
    for (const QJsonValue& v : given.isArray() ? given.toArray() : given.isUndefined() ? QJsonArray{} : QJsonArray{given})
        files << s.media(v.toString()).path;
    if (files.isEmpty())
        for (const MediaInfo& m : s.project.media())
            if (m.hasAudio && !m.transcripts.isEmpty()) files << m.path;
    if (files.isEmpty())
        fail("BAD_ARGUMENT", "missing 'media' (the recordings to cut from); no file of the project has a transcript yet");
    return files;
}

QString scriptOf(Session& s, const QJsonObject& op)
{
    if (op.contains("script_file")) {
        QFile f(absolute(op.value("script_file").toString()));
        if (!f.open(QIODevice::ReadOnly)) fail("NOT_FOUND", "cannot read script_file " + f.fileName());
        return QString::fromUtf8(f.readAll());
    }
    const QString text = op.value("script").toString();
    if (text.trimmed().isEmpty()) fail("BAD_ARGUMENT", "missing 'script' (the text, sentences ending with . ! ?) or 'script_file'");
    return text;
}

void opRoughCut(Session& s, const QJsonObject& op)
{
    const QVector<ScriptAlign::Sentence> script = ScriptAlign::sentences(scriptOf(s, op));
    if (script.isEmpty()) fail("BAD_ARGUMENT", "the script has no words");
    const QString pickName = op.value("takes").toString("last_good");
    if (pickName != "last_good" && pickName != "best") fail("BAD_ARGUMENT", "takes must be 'last_good' or 'best'");
    const ScriptAlign::Pick pick = pickName == "best" ? ScriptAlign::Pick::Best : ScriptAlign::Pick::LastGood;
    const QStringList files = roughCutMedia(s, op);
    // Every file's words: its transcript in the project, else made now (whisper, made before the app gets the edit
    // when live: _transcripts) and kept as the file's transcript (the Transcript panel shows it, best_takes uses it)
    const QJsonObject prepared = op.value("_transcripts").toObject();
    QVector<QVector<ScriptAlign::Word>> sources;
    QStringList made;
    for (const QString& file : files) {
        const MediaInfo* m = s.project.mediaInfo(file);
        if (m && !m->transcripts.isEmpty()) {
            sources << wordsOf(m->transcripts.first());
            continue;
        }
        MediaTranscript t;
        if (prepared.contains(file)) {
            const QJsonObject p = prepared.value(file).toObject();
            t.language = p.value("language").toString();
            for (const QJsonValue& v : p.value("words").toArray())
                t.words << TranscriptWord{qint64(v[0].toDouble()), qint64(v[1].toDouble()), v[2].toString()};
        } else if (!s.live && canTranscribe()) {
            t.words = transcribeFile(file, op.value("language").toString(), &t.language);
        } else {
            fail("NO_TRANSCRIPT", QString("%1 has no transcript: transcribe it in the app's Transcript panel, or install "
                                          "whisper (`extensions`) and rough_cut transcribes it")
                                      .arg(QFileInfo(file).fileName()));
        }
        s.project.setTranscript(file, t);
        made << QFileInfo(file).fileName();
        sources << wordsOf(t);
    }
    const QVector<ScriptAlign::Choice> choices = ScriptAlign::roughCut(script, sources, pick);

    // A new timeline of the chosen takes in the script's order (the timeline before stays as it is)
    const double fps = s.format().rate.fps();
    // A free name: the wanted one, else numbered ("Rohschnitt 2")
    const QString wanted = op.value("name").toString(T("Rohschnitt"));
    QSet<QString> names;
    for (const Sequence& q : s.project.sequences()) names.insert(s.project.sequenceName(q.id));
    QString name = wanted;
    for (int n = 2; names.contains(name); ++n) name = QString("%1 %2").arg(wanted).arg(n);
    applyOp(s, QJsonObject{{"op", "new_timeline"}, {"name", name}});
    QJsonArray sentencesOut, missing;
    const auto takeJson = [&](const ScriptAlign::Take& t) {
        return QJsonObject{{"file", files[t.source]}, {"from_s", round2(t.from)}, {"to_s", round2(t.to)},
                           {"coverage", round2(t.coverage)}, {"good", t.good}};
    };
    for (const ScriptAlign::Choice& c : choices) {
        QJsonObject o{{"sentence", script[c.sentence].text}, {"why", c.why}};
        if (c.chosen < 0) {
            missing << script[c.sentence].text;
            sentencesOut << o;
            continue;
        }
        const ScriptAlign::Take& t = c.takes[c.chosen];
        const QVector<ScriptAlign::Word>& words = sources[t.source];
        // From just before the first word to the end of the last one spoken (not into the next word)
        double from = std::max(0.0, t.from - 0.08);
        if (t.first > 0) from = std::max(from, words[t.first - 1].to);
        double to = std::min(t.to, words[t.last].from + 0.25 + 0.07 * words[t.last].text.size()) + 0.12;
        if (t.last + 1 < words.size()) to = std::min(to, words[t.last + 1].from);
        if (to - from < 1.0 / fps) to = from + 1.0 / fps;
        applyOp(s, QJsonObject{{"op", "keep"}, {"media", files[t.source]},
                               {"ranges", QJsonArray{QJsonArray{QString("%1s").arg(from, 0, 'f', 3), QString("%1s").arg(to, 0, 'f', 3)}}}});
        o["take"] = takeJson(t);
        QJsonArray alternatives;
        for (int k = 0; k < c.takes.size(); ++k)
            if (k != c.chosen) alternatives << takeJson(c.takes[k]);
        if (!alternatives.isEmpty()) o["alternatives"] = alternatives;
        sentencesOut << o;
    }
    if (missing.size() == choices.size())
        fail("NOT_FOUND", "no sentence of the script was found in the transcripts (other language? other files?)");
    QJsonObject r{{"timeline", name}, {"sentences", sentencesOut}, {"missing", missing}};
    if (!made.isEmpty()) r["transcribed"] = QJsonArray::fromStringList(made);
    s.notes << QJsonObject{{"rough_cut", r}};
}

void opBestTakes(Session& s, const QJsonObject& op)
{
    const Timeline& tl = s.project.timeline();
    int track = -1;
    if (op.contains("track")) {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
    }
    QSet<int> only;
    if (op.contains("clips") || op.contains("clip"))
        for (int id : clipIds(s, op)) only.insert(id);
    const double fps = s.format().rate.fps();
    QVector<TranscriptCut::Word> spoken = TranscriptCut::timelineWords(tl, s.project.media(), fps, track);
    if (!only.isEmpty())
        spoken.erase(std::remove_if(spoken.begin(), spoken.end(), [&](const TranscriptCut::Word& w) {
                         if (only.contains(w.clipId)) return false;
                         const Clip* c = TimelineOps::findClip(tl, w.clipId);
                         return !(c && c->linkId && std::any_of(only.begin(), only.end(), [&](int id) {
                             const Clip* o = TimelineOps::findClip(tl, id);
                             return o && o->linkId == c->linkId;
                         }));
                     }),
                     spoken.end());
    if (spoken.isEmpty())
        fail("NO_TRANSCRIPT", "no transcribed speech on the timeline (transcribe the clips' files first: the app's "
                              "Transcript panel, or rough_cut)");
    QVector<QPair<int, int>> cut;
    QJsonArray out;
    // Per audio track (two speakers who both say "Ja, genau." are no repeat), words up to their spoken end
    QVector<int> tracks;
    for (const TranscriptCut::Word& w : spoken)
        if (!tracks.contains(w.track)) tracks << w.track;
    for (int tr : tracks) {
        QVector<TranscriptCut::Word> on;
        for (const TranscriptCut::Word& w : spoken)
            if (w.track == tr) on << w;
        QVector<ScriptAlign::Word> words;
        for (const TranscriptCut::Word& w : on) words << ScriptAlign::Word{w.text, w.from / fps, std::max(w.from, w.spokenTo) / fps};
        const auto text = [&](const ScriptAlign::Take& t) {
            QStringList w;
            for (int i = t.first; i <= t.last; ++i) w << on[i].text;
            return w.join(' ');
        };
        for (const ScriptAlign::Repeat& r : ScriptAlign::repeats(words)) {
            QJsonArray removed;
            for (int k = 0; k < r.takes.size(); ++k) {
                if (k == r.keep) continue;
                const ScriptAlign::Take& t = r.takes[k];
                // Up to the next word when it follows within a second in the same clip (the pause goes with it),
                // else just after the last word: never on into another clip or a long stretch without words
                const TranscriptCut::Word& last = on[t.last];
                const int spokenEnd = std::max(last.from + 1, last.spokenTo);
                int end = std::min(last.to, spokenEnd + int(std::lround(0.15 * fps)));
                if (t.last + 1 < on.size() && on[t.last + 1].clipId == last.clipId
                    && on[t.last + 1].from - spokenEnd <= int(std::lround(fps)))
                    end = on[t.last + 1].from;
                cut << qMakePair(on[t.first].from, end);
                removed << QJsonObject{{"text", text(t)}, {"from_s", round2(t.from)}, {"to_s", round2(t.to)}};
            }
            const ScriptAlign::Take& kept = r.takes[r.keep];
            out << QJsonObject{{"kept", QJsonObject{{"text", text(kept)}, {"from_s", round2(kept.from)}, {"to_s", round2(kept.to)}}},
                               {"removed", removed}, {"track", QString("A%1").arg(tr + 1)}};
        }
    }
    if (cut.isEmpty()) {
        s.notes << "best_takes: no sentence was said twice in a row - nothing cut";
        return;
    }
    s.editor.deleteRanges(cut, true, T("Beste Takes behalten"));
    s.notes << QJsonObject{{"best_takes", out}};
}

// ---------- hide_jumps ----------

// Where the subject of the clip is (0..1 of the picture, the mean of the reframe samples in the clip's part of the
// file, weighted by how sure they are); nullopt = not measured
std::optional<QPointF> subjectOf(Session& s, const Clip& c, bool measure)
{
    const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
    const auto n = ReframeAnalysis::need(c, s.project.format().rate, m ? m->length : 0);
    if (!n) return std::nullopt;
    std::optional<ReframeAnalysis::Samples> samples = ReframeAnalysis::cached(*n);
    if (!samples && measure) samples = ReframeAnalysis::analyze(*n);
    if (!samples) return std::nullopt;
    double x = 0, y = 0, w = 0;
    for (const ReframeAnalysis::Sample& smp : *samples)
        if (smp.seconds >= n->from && smp.seconds <= n->to && smp.weight > 0) {
            x += smp.x * smp.weight;
            y += smp.y * smp.weight;
            w += smp.weight;
        }
    if (w <= 0) return std::nullopt;
    return QPointF(x / w, y / w);
}

void opHideJumps(Session& s, const QJsonObject& op)
{
    const Timeline& tl = s.project.timeline();
    const double zoom = op.contains("zoom") ? numberOf(op, "zoom", 1.01, 2.0) : 1.12;
    const int minSkip = op.contains("min_jump") ? timeOf(s, op, "min_jump") : int(std::lround(0.3 * s.format().rate.fps()));
    const QString center = op.value("center").toString("subject");
    if (center != "subject" && center != "middle") fail("BAD_ARGUMENT", "center must be 'subject' or 'middle'");
    // The track: given, else the one of the clips given, else V1
    int track = 0;
    QSet<int> only;
    if (op.contains("clips") || op.contains("clip"))
        for (int id : onTracks(s, clipIds(s, op), TrackKind::Video, "hide_jumps")) only.insert(id);
    if (op.contains("track")) {
        TrackKind k = TrackKind::Video;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Video) fail("BAD_ARGUMENT", "track must be a video track (V1 …)");
    } else if (!only.isEmpty()) {
        TrackRef ref;
        TimelineOps::findClip(tl, *only.begin(), &ref);
        track = ref.index;
    }
    if (track < 0 || track >= tl.video.size()) fail("NOT_FOUND", QString("no track V%1").arg(track + 1));
    const QVector<JumpCut::Jump> jumps = JumpCut::find(tl.video[track], minSkip);
    // Runs of clips joined by jumps: within each run every second clip is pushed in (the first stays as it is)
    QHash<int, int> next; // left clip -> right clip of a jump
    QSet<int> rights;
    for (const JumpCut::Jump& j : jumps) {
        next.insert(j.left, j.right);
        rights.insert(j.right);
    }
    QVector<int> zoomed, skipped;
    for (const JumpCut::Jump& j : jumps) {
        if (rights.contains(j.left)) continue; // not the start of a run
        bool in = false;
        for (int id = j.left; id; id = next.value(id)) {
            if (in && (only.isEmpty() || only.contains(id))) {
                const Clip* c = TimelineOps::findClip(tl, id);
                const bool free = c && !TimelineOps::isLocked(tl, id) && c->transform.zoomX == 1.0 && c->transform.zoomY == 1.0 && c->transform.posX == 0
                                  && c->transform.posY == 0
                                  && !Keys::animated(*c, AnimParam::ZoomX) && !Keys::animated(*c, AnimParam::ZoomY)
                                  && !Keys::animated(*c, AnimParam::PosX) && !Keys::animated(*c, AnimParam::PosY);
                (free ? zoomed : skipped) << id;
            }
            in = !in;
        }
    }
    if (jumps.isEmpty()) {
        s.notes << QString("hide_jumps: no jump cut on V%1 (clips of one file that touch and skip at least %2 of it)")
                       .arg(track + 1)
                       .arg(tc(minSkip, s.format()));
        return;
    }
    const ProjectFormat f = s.format();
    int onSubject = 0;
    for (int id : zoomed) {
        const Clip c = *TimelineOps::findClip(s.project.timeline(), id);
        // Pushed in around the subject: it stays where it is in the frame; the zoomed picture still fills the frame
        QPointF shift;
        if (center == "subject")
            if (const auto p = subjectOf(s, c, !s.live)) {
                const double px = (p->x() - 0.5) * f.width, py = (0.5 - p->y()) * f.height; // Y up
                const double limX = (zoom - 1) * f.width / 2, limY = (zoom - 1) * f.height / 2;
                shift = QPointF(std::clamp((1 - zoom) * px, -limX, limX), std::clamp((1 - zoom) * py, -limY, limY));
                ++onSubject;
            }
        s.editor.modifyClips({id}, T("Sprünge kaschieren"), [&](Clip& clip) {
            if (!clip.transform.transformOn) { // values kept while it was off (a rotation) would come back
                clip.transform.rotation = 0;
                clip.transform.transformOn = true;
            }
            clip.transform.zoomX = clip.transform.zoomY = zoom;
            clip.transform.posX = std::round(shift.x() * 10) / 10;
            clip.transform.posY = std::round(shift.y() * 10) / 10;
        });
    }
    QStringList ids;
    for (int id : zoomed) ids << QString::number(id);
    QString note = QString("hide_jumps: %1 jump cut%2 on V%3, zoom %4 on clip%5 %6")
                       .arg(jumps.size())
                       .arg(jumps.size() == 1 ? "" : "s")
                       .arg(track + 1)
                       .arg(zoom, 0, 'f', 2)
                       .arg(zoomed.size() == 1 ? "" : "s", ids.join(", "));
    if (center == "subject")
        note += QString(" (%1 of %2 around the subject%3)")
                    .arg(onSubject)
                    .arg(zoomed.size())
                    .arg(onSubject < zoomed.size() ? (s.live ? ", the others in the middle: not measured yet (reframe measures it)"
                                                             : ", the others in the middle")
                                                   : QString());
    if (!skipped.isEmpty()) {
        QStringList sk;
        for (int id : skipped) sk << QString::number(id);
        note += ". Left as they are (own zoom/position, keyframes or a locked track): " + sk.join(", ");
    }
    s.notes << note;
}

// ---------- broll ----------

// The spoken sentences of the timeline in [from, to): timeline frames and their text
struct Said {
    int from = 0, to = 0;
    QString text;
};

QVector<Said> saidIn(Session& s, int from, int to)
{
    QVector<Said> out;
    Said cur;
    const auto flush = [&] {
        if (!cur.text.trimmed().isEmpty()) out << cur;
        cur = Said{};
    };
    for (const TranscriptCut::Word& w : TranscriptCut::timelineWords(s.project.timeline(), s.project.media(), s.format().rate.fps())) {
        if (w.from < from || w.from >= to) continue;
        if (cur.text.isEmpty()) cur.from = w.from;
        cur.to = w.spokenTo > w.from ? w.spokenTo : w.to;
        cur.text += w.text + ' ';
        const QString t = w.text.trimmed();
        if (t.endsWith('.') || t.endsWith('!') || t.endsWith('?')) flush();
    }
    flush();
    return out;
}

// The file playing on the lowest video track at the frame (the one the B-roll covers), or empty
QString playingAt(const Timeline& tl, int frame)
{
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips)
            if (c.enabled && !c.mediaPath.isEmpty() && c.start <= frame && frame < c.end()) return c.mediaPath;
    return {};
}

void opBroll(Session& s, const QJsonObject& op)
{
    const Timeline& tl = s.project.timeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) s.failEmpty();
    const int from = op.contains("from") ? timeOf(s, op, "from") : 0;
    const int to = op.contains("to") ? std::min(end, timeOf(s, op, "to")) : end;
    if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
    TrackKind kind = TrackKind::Video;
    const int track = op.contains("track") ? parseTrack(op.value("track"), &kind) : 1;
    if (kind != TrackKind::Video || track < 1) fail("BAD_ARGUMENT", "track must be a video track above V1 (default V2)");
    const QString query = op.value("query").toString().trimmed();
    const int most = op.contains("max") ? std::clamp(op.value("max").toInt(), 1, 100) : 10;
    // What can be shown: the indexed shots of the project's video files (cached; made now on a file, not in the app)
    QVector<IndexSearch::Shot> shots;
    QStringList notIndexed;
    for (const MediaInfo& m : s.project.media()) {
        if (!m.hasVideo || m.isImage) continue;
        std::optional<MediaIndex::Entry> e = MediaIndex::cached(m.path);
        if (!e && !s.live) e = MediaIndex::build(m.path);
        if (!e) {
            notIndexed << QFileInfo(m.path).fileName();
            continue;
        }
        if (const MediaTranscript* t = m.transcript(0)) MediaIndex::addSpeech(*e, t->words);
        shots += MediaIndex::searchable(*e);
    }
    if (shots.isEmpty())
        fail("NOT_FOUND", "no indexed shots: run `index` on the project first (and describe the shots with annotate)");
    // What is said: a sentence each (or the whole range for a given query)
    QVector<Said> said = query.isEmpty() ? saidIn(s, from, to) : QVector<Said>{Said{from, to, query}};
    if (said.isEmpty())
        fail("NO_TRANSCRIPT", "nothing transcribed is said in the range: give query, or transcribe the speech first");
    QJsonArray placed, unmatched;
    QSet<QString> used; // file + shot: nothing twice
    const double fps = s.format().rate.fps();
    for (const Said& line : said) {
        if (placed.size() >= most) break;
        const QString running = playingAt(tl, line.from);
        const IndexSearch::Hit* best = nullptr;
        const QVector<IndexSearch::Hit> hits = IndexSearch::search(line.text, shots, 0);
        for (const IndexSearch::Hit& h : hits) {
            if (h.score <= 0 || h.file == running || used.contains(h.file + '#' + QString::number(h.n))) continue;
            if (const MediaInfo* mi = s.project.mediaInfo(h.file); !mi || !mi->hasVideo || mi->isImage) continue;
            // Words of the question, not only a quality ("calm") or an event, must have matched
            const bool worded = std::any_of(h.reasons.begin(), h.reasons.end(), [](const IndexSearch::Reason& r) {
                return r.field == "tag" || r.field == "desc" || r.field == "speech";
            });
            if (!worded) continue;
            best = &h;
            break;
        }
        if (!best) {
            unmatched << line.text.trimmed();
            continue;
        }
        used.insert(best->file + '#' + QString::number(best->n));
        // As long as the sentence (the shot permitting), from the shot's start
        const int length = std::max(1, std::min(line.to - line.from, int(std::floor((best->to - best->from) * fps))));
        // Picture only, onto the first unlocked video track from `track` up that is free there: a place op would
        // also lay its sound over the audio track of that number and overwrite what lies on either track
        const MediaInfo& m = s.media(best->file);
        const Timeline& now = s.project.timeline();
        int vt = track;
        for (; vt < now.video.size(); ++vt) {
            const Track& t = now.video[vt];
            if (t.locked) continue;
            if (std::none_of(t.clips.begin(), t.clips.end(),
                             [&](const Clip& c) { return c.start < line.from + length && c.end() > line.from; }))
                break;
        }
        Clip b;
        b.kind = ClipKind::Media;
        b.mediaPath = m.path;
        b.start = line.from;
        b.in = std::clamp(int(std::lround(best->from * fps)), 0, std::max(0, m.length - 1));
        b.out = std::min(m.length - 1, b.in + length - 1);
        b.pictureAspect = m.pictureAspect;
        if (b.out < b.in) continue;
        b.id = s.project.newClipId();
        s.project.edit(T("Einfügen"), [&](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Video, vt + 1);
            TimelineOps::placeClip(tl.video[vt], b, [&s] { return s.project.newClipId(); });
        });
        placed << QJsonObject{{"said", line.text.trimmed()}, {"at", line.from}, {"at_s", round2(line.from / fps)},
                              {"length_s", round2(length / fps)}, {"file", best->file}, {"shot", best->n},
                              {"from_s", round2(best->from)}, {"track", QString("V%1").arg(vt + 1)}, {"why", best->why}};
    }
    QJsonObject r{{"placed", placed}, {"track", QString("V%1").arg(track + 1)}};
    if (!unmatched.isEmpty()) r["no_match"] = unmatched;
    if (!notIndexed.isEmpty()) r["not_indexed"] = QJsonArray::fromStringList(notIndexed);
    if (placed.isEmpty()) r["note"] = "no shot matches what is said: describe the shots (`index` annotate) or give query";
    s.notes << QJsonObject{{"broll", r}};
}

} // namespace

QJsonObject prepareTalk(const QJsonObject& editArgs, Context& ctx)
{
    Q_UNUSED(ctx);
    QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    bool any = false;
    for (const QJsonValue& v : ops) any = any || v.toObject().value("op").toString() == "rough_cut";
    if (!any || !canTranscribe()) return editArgs;
    try {
        Session s(editArgs.value("project").toString()); // a copy of what the app shows
        for (int i = 0; i < ops.size(); ++i) {
            QJsonObject op = ops[i].toObject();
            if (op.value("op").toString() != "rough_cut" || op.contains("_transcripts")) continue;
            QJsonObject prepared;
            for (const QString& file : roughCutMedia(s, op)) {
                const MediaInfo* m = s.project.mediaInfo(file);
                if (m && !m->transcripts.isEmpty()) continue;
                QString lang;
                QJsonArray words;
                for (const TranscriptWord& w : transcribeFile(file, op.value("language").toString(), &lang))
                    words << QJsonArray{double(w.from), double(w.to), w.text};
                prepared[file] = QJsonObject{{"language", lang}, {"words", words}};
            }
            if (prepared.isEmpty()) continue;
            op["_transcripts"] = prepared;
            ops[i] = op;
        }
    } catch (const Error&) {
        return editArgs; // the op reports it where it runs
    }
    QJsonObject out = editArgs;
    out["ops"] = ops;
    return out;
}

void addTalkOps(QVector<OpDef>& ops)
{
    ops << OpDef{"jl_cut", "{op:'jl_cut', at|clip, audio, edge?}",
                 "J/L cut: only the sound edge of a picture cut moves (audio:'-0.5s' = J, the next clip's sound starts "
                 "early; '+0.5s' = L, the previous sound runs on; 0 = back in sync)",
                 "at = a time near the cut (the nearest picture cut whose clips both have linked sound), or clip = the "
                 "clip after the cut (edge:'end' = the clip before it; a sound clip stands for its linked picture). "
                 "audio is measured from the picture cut, not from where the sound cut is now: running it again with "
                 "another value replaces it. The picture, the links and the sync of everything else stay; the sound "
                 "clips on every audio track where the two linked sounds meet roll together (one undo step). Not "
                 "enough material in the files: an error with how far it can go. A sound cut alone (no picture): "
                 "trim mode:'roll' on the sound clip.",
                 opJlCut}
        << OpDef{"room_tone", "{op:'room_tone', track, from?, to?}",
                 "fill the gaps of a dialogue track with the quiet of the same recording (room tone), so pause cuts do "
                 "not drop into dead silence",
                 "track = the audio track (\"A1\"); from/to limit it to a range (default: every gap between two clips). "
                 "Each gap gets a loop of the quietest steady 1-2 s of the recording before it (the one after it when "
                 "the one before is a title or retimed), measured in the file around the parts the track plays; its "
                 "level is matched to the take's noise floor (10th percentile of its 50 ms levels) and it gets the "
                 "take's volume, pan and audio effects. Seams are crossfades that start on the cut (about 20 ms, at "
                 "least one frame); the takes fade into the room tone without touching their sound before the cut. "
                 "Only measured by level (no speech recognition): a recording without a quiet stretch of 1 s (all "
                 "words, digital silence) is left out with a note. Nothing happens on its own: only when asked.",
                 opRoomTone, {}}
        << OpDef{"rough_cut", "{op:'rough_cut', script|script_file, media?:[files], takes?:'last_good'|'best', language?, name?}",
                 "a rough cut from a script: each sentence's take from what was said, in the script's order, as a new "
                 "timeline",
                 "The sentences (split after . ! ?) are found in the transcripts of the media (default: every file of "
                 "the project with one) by a word alignment that forgives misheard words; a sentence said several times "
                 "has takes: last_good (default) = the last complete one without extra words (people repeat until it "
                 "is right), best = the highest rated (complete, no slip, few fillers and pauses). A file without a "
                 "transcript is transcribed with whisper when it is installed (language) and keeps it. The answer's "
                 "notes give rough_cut {timeline, sentences:[{sentence, take:{file, from_s, to_s, coverage, good}, "
                 "alternatives, why}], missing:[sentences not found], transcribed}. With plan:true it is a plan.",
                 opRoughCut, {"_transcripts"}}
        << OpDef{"best_takes", "{op:'best_takes', clips?, track?}",
                 "without a script: a sentence said again right after itself (a slip, a false start) keeps only its "
                 "last good take",
                 "From the transcripts of the timeline's clips (all audio tracks, or track / the clips given); the "
                 "earlier takes are cut out with ripple (one undo step). The answer's notes give best_takes "
                 "[{kept:{text, from_s, to_s}, removed:[…]}] in source seconds of the words.",
                 opBestTakes, kClipKeys}
        << OpDef{"hide_jumps", "{op:'hide_jumps', clips?, track?, zoom?:1.12, min_jump?:'0.3s', center?:'subject'|'middle'}",
                 "after a pause or word cut: every second clip of a jump cut pushed in (zoom), so the jump looks like a "
                 "camera change",
                 "A jump = two clips of one track that touch, play the same file at normal speed and skip at least "
                 "min_jump of it (another file is a real cut and stays). In each run of jumps the clips alternate "
                 "between as they are and zoom (1.01-2); center subject (default) keeps the subject (the reframe "
                 "measurement, cached) where it is in the frame, middle zooms on the centre. Clips with their own zoom, "
                 "position or keyframes are left out (named in the answer). track = the video track (default: the one "
                 "of the clips given, else V1). One undo step; `check` reports remaining jumps as jump_cut (info).",
                 opHideJumps, kClipKeys}
        << OpDef{"broll", "{op:'broll', from?, to?, query?, track?:'V2', max?}",
                 "B-roll that matches what is said: shots of the material index over the sentences of the transcript",
                 "Each spoken sentence in [from, to) (the timeline's transcripts) is searched in the index (`index`: "
                 "descriptions, tags, speech of every video file of the project; build it and describe the shots "
                 "first) and the best shot whose words match - not from the file playing below, no shot twice - goes "
                 "onto track (default V2) as long as the sentence (or the shot), picture only. query instead searches "
                 "that once for the whole range. At most max (10) shots. The answer's notes give broll {placed:[{said, "
                 "at_s, length_s, file, shot, from_s, why}], no_match, not_indexed}. Best as a plan (plan:true): the "
                 "user picks.",
                 opBroll};
}

} // namespace Cli::detail
