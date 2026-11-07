// SPDX-License-Identifier: AGPL-3.0-only
// Readable subtitle cues (0.19, Subtitles::readable): whisper's fast one-line pieces regrouped into 1-2 line cues
// (lines at most 42 characters, 2 lines, at most ~17-20 characters per second where the speech leaves room, at
// least 0.8 s, a new cue at a sentence end, never across a pause > 0.7 s, two lines split at a comma), word times
// kept, every word once in order; `check` no longer finds them too fast; the edit op 'subtitles' with readable,
// words and an existing track (one undo step).
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Subtitles.h"
#include "core/Transcript.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>

namespace {

const double kFps = 25;
const ProjectFormat kFormat{1920, 1080, {25, 1}};

// Speech of about 15 characters per second: words of `text` one after the other (100 ms + 60 ms per letter, 30 ms
// apart, 300 ms after a sentence); "|" = a pause of 1 s
QVector<Transcript::Word> speech(const QString& text, qint64 at = 0)
{
    QVector<Transcript::Word> out;
    for (const QString& w : text.split(' ', Qt::SkipEmptyParts)) {
        if (w == "|") {
            at += 1000;
            continue;
        }
        Transcript::Word word;
        word.text = w;
        word.from = at;
        word.to = at + 100 + 60 * w.size();
        at = word.to + (w.endsWith('.') || w.endsWith('?') ? 300 : 30);
        out << word;
    }
    return out;
}

const QString kText =
    "So, heute bauen wir zusammen einen kleinen Tisch aus altem Holz. Zuerst messen wir die Bretter, dann sägen wir "
    "sie auf die richtige Länge, und danach schleifen wir alles schön glatt. | Das dauert nicht lange. Ja. Gut, "
    "weiter geht's mit den Beinen, die wir aus den dicken Latten schneiden, damit der Tisch später nicht wackelt.";

QStringList wordsOf(const QVector<SubtitleCue>& cues)
{
    QStringList out;
    for (const SubtitleCue& c : cues) out += c.text.simplified().split(' ', Qt::SkipEmptyParts);
    return out;
}

double cps(const SubtitleCue& c)
{
    QString letters = c.text;
    letters.remove('\n');
    return letters.trimmed().size() / (c.length() / kFps);
}

int fastCount(const QVector<SubtitleCue>& cues)
{
    Timeline tl;
    tl.video.resize(1);
    SubtitleTrack st;
    st.cues = cues;
    tl.subtitles << st;
    int n = 0;
    for (const ExportCheck::Issue& i : ExportCheck::timeline(tl, 0, cues.last().end + 50, kFormat))
        n += i.check == "subtitle_fast";
    return n;
}

// The rules hold for every cue of `out` made from `words`
void checkRules(const QVector<SubtitleCue>& out, const QVector<Transcript::Word>& words, bool wordTimes)
{
    QStringList all;
    for (const auto& w : words) all << w.text;
    CHECK_EQ(wordsOf(out), all);
    const int pause = int(std::lround(0.7 * kFps));
    int k = 0; // index of the cue's first word in `words`
    double worst = 0;
    for (int i = 0; i < out.size(); ++i) {
        const SubtitleCue& c = out[i];
        const QStringList lines = c.text.split('\n');
        CHECK(lines.size() <= 2);
        for (const QString& l : lines) CHECK(l.size() <= 42);
        if (i > 0) CHECK(c.start >= out[i - 1].end);
        const int n = int(c.text.simplified().split(' ').size());
        // Never across a pause
        for (int j = k + 1; j < k + n; ++j) {
            const int gap = int(std::lround(words[j].from / 1000.0 * kFps)) - int(std::lround(Transcript::spokenEnd(words[j - 1]) / 1000.0 * kFps));
            if (!CHECK(gap <= pause)) std::printf("       across a pause: %s\n", qPrintable(c.text));
        }
        // Long and slow enough where the next cue leaves room
        const int room = i + 1 < out.size() ? out[i + 1].start - c.start : 1 << 20;
        if (room >= int(0.8 * kFps)) CHECK(c.length() >= int(0.8 * kFps) - 1);
        const int needed = int(std::ceil(c.text.size() / 17.0 * kFps));
        if (room >= needed) {
            worst = std::max(worst, cps(c));
            if (!CHECK(cps(c) <= 17.5)) std::printf("       too fast (%.1f): %s\n", cps(c), qPrintable(c.text));
        }
        if (wordTimes && CHECK_EQ(int(c.words.size()), n))
            for (int j = 0; j < n; ++j)
                CHECK_EQ(c.start + c.words[j].from, int(std::lround(words[k + j].from / 1000.0 * kFps)));
        k += n;
    }
    std::printf("       %d cues, slowest allowed 17, fastest with room %.1f characters/s\n", int(out.size()), worst);
}

void testFromWords()
{
    std::printf("from words\n");
    const QVector<Transcript::Word> words = speech(kText);
    // As `transcribe` makes them without readable: one line
    const QVector<SubtitleCue> old = Transcript::toCues(words, kFps);
    double fastest = 0;
    for (const SubtitleCue& c : old) fastest = std::max(fastest, cps(c));
    std::printf("       one-line cues: %d, fastest %.1f characters/s, %d too fast for check\n", int(old.size()),
                fastest, fastCount(old));
    const QVector<SubtitleCue> out = Subtitles::readable(Transcript::wordCues(words, kFps), kFps);
    for (const SubtitleCue& c : out) std::printf("       [%5.2f-%5.2f] %s\n", c.start / kFps, c.end / kFps, qPrintable(QString(c.text).replace('\n', " / ")));
    checkRules(out, words, true);
    CHECK_EQ(fastCount(out), 0);
    CHECK(out.size() < old.size());
    // Some cues use two lines, split evenly
    bool two = false;
    for (const SubtitleCue& c : out)
        if (c.text.contains('\n')) {
            two = true;
            const QStringList l = c.text.split('\n');
            CHECK(std::abs(l[0].size() - l[1].size()) <= 20);
        }
    CHECK(two);
    // The pause stays a break: "glatt." ends a cue, "Das" starts one
    bool pauseBreak = false;
    for (int i = 0; i + 1 < out.size(); ++i)
        pauseBreak |= out[i].text.endsWith("glatt.") && out[i + 1].text.startsWith("Das");
    CHECK(pauseBreak);
}

void testFromCues()
{
    std::printf("from one-line cues without word times\n");
    // Whisper pieces of 23-29 characters shown 1 s each, 0.6 s apart: too fast to read one by one
    QVector<SubtitleCue> cues;
    const QStringList pieces{"Das ist wirklich unglaublich", "schnell gesprochen, oder", "nicht? Ich finde schon,",
                             "dass man das lesen können", "muss, sonst bringt es nichts."};
    for (int i = 0; i < pieces.size(); ++i) cues << SubtitleCue{0, i * 40, i * 40 + 25, pieces[i]};
    CHECK(fastCount(cues) > 0);
    const QVector<SubtitleCue> out = Subtitles::readable(cues, kFps);
    for (const SubtitleCue& c : out) std::printf("       [%5.2f-%5.2f] %s\n", c.start / kFps, c.end / kFps, qPrintable(QString(c.text).replace('\n', " / ")));
    QStringList all;
    for (const SubtitleCue& c : cues) all += c.text.split(' ');
    CHECK_EQ(wordsOf(out), all);
    for (const SubtitleCue& c : out) {
        CHECK(c.text.split('\n').size() <= 2);
        for (const QString& l : c.text.split('\n')) CHECK(l.size() <= 42);
        CHECK(c.words.isEmpty()); // no word times known
    }
    CHECK(out.size() < cues.size());
    CHECK_EQ(out.first().start, 0);
    CHECK(out.last().end >= 185); // at least as long as the last piece
    CHECK_EQ(fastCount(out), 0);
}

void testPausesAndSentences()
{
    std::printf("pauses, sentences, comma\n");
    // Two short phrases with a long pause: never one cue, each at least 0.8 s
    const QVector<Transcript::Word> w = speech("Ja. | | Okay.");
    const QVector<SubtitleCue> out = Subtitles::readable(Transcript::wordCues(w, kFps), kFps);
    if (CHECK_EQ(out.size(), 2)) {
        CHECK(out[0].length() >= 20);
        CHECK(out[1].length() >= 20);
    }
    // A slow sentence ends its cue
    QVector<Transcript::Word> slow{{0, 900, "Hallo"}, {1000, 1900, "zusammen."}};
    slow += speech("Heute bauen wir einen Tisch.", 2300);
    const QVector<SubtitleCue> s = Subtitles::readable(Transcript::wordCues(slow, kFps), kFps);
    if (CHECK(s.size() >= 2)) {
        CHECK_EQ(s[0].text, QString("Hallo zusammen."));
        CHECK(s[1].text.startsWith("Heute"));
    }
    // Two lines split at the comma when it is near the middle
    const QVector<SubtitleCue> c = Subtitles::readable(
        Transcript::wordCues(speech("Wenn du morgen Zeit hast, kommen wir alle zu dir rüber und kochen"), kFps), kFps);
    if (CHECK_EQ(c.size(), 1)) CHECK_EQ(c[0].text, QString("Wenn du morgen Zeit hast,\nkommen wir alle zu dir rüber und kochen"));
    // Other rules: one line of 30
    Subtitles::ReadableRules r;
    r.lines = 1;
    r.lineChars = 30;
    for (const SubtitleCue& x : Subtitles::readable(Transcript::wordCues(speech(kText), kFps), kFps, r)) {
        CHECK(!x.text.contains('\n'));
        CHECK(x.text.size() <= 30);
    }
}

void testOverlapsAndSound()
{
    std::printf("overlapping cues, quiet words\n");
    // Overlapping cues (an SRT of two speakers): cut apart, no flash of a frame or two
    const QVector<SubtitleCue> overlap{{0, 0, 60, "Das ist der erste Satz"},   {0, 40, 100, "und hier spricht jemand anders"},
                                       {0, 40, 70, "gleichzeitig"},           {0, 95, 160, "noch ein Satz zum Schluss."}};
    const QVector<SubtitleCue> out = Subtitles::readable(overlap, kFps);
    for (const SubtitleCue& c : out) std::printf("       [%5.2f-%5.2f] %s\n", c.start / kFps, c.end / kFps, qPrintable(QString(c.text).replace('\n', " / ")));
    for (int i = 0; i < out.size(); ++i) {
        CHECK(out[i].length() >= 20);
        if (i > 0) CHECK(out[i].start >= out[i - 1].end);
    }
    CHECK_EQ(wordsOf(out).size(), 5 + 5 + 1 + 5);
    // Without text: nothing
    CHECK(Subtitles::readable({{0, 0, 25, " "}, {0, 30, 50, ""}}, kFps).isEmpty());

    // `transcribe` readable: a quiet word between loud ones stays (its own sound is too quiet to count), a cue in
    // silence (whisper made it up) goes, word times stay where the words are spoken
    const QVector<Transcript::Word> w = speech("Ich war gestern ja auch dabei. | | | Hallo?");
    const qint64 quiet = w[3].from, quietEnd = w[3].to, last = w.last().from;
    const Transcript::Levels levels = [&] {
        Transcript::Levels l;
        for (qint64 t = 0; t < last + 1000; t += 10)
            l.db << ((t < w[4].to + 100 && !(t >= quiet && t < quietEnd)) ? -20.f : -90.f);
        return l;
    }();
    const QVector<SubtitleCue> cues = Transcript::readableCues(w, levels, kFps);
    if (CHECK_EQ(cues.size(), 1)) {
        CHECK_EQ(cues[0].text, QString("Ich war gestern ja auch dabei."));
        if (CHECK_EQ(cues[0].words.size(), 6))
            for (int i = 0; i < 6; ++i) CHECK_EQ(cues[0].start + cues[0].words[i].from, int(std::lround(w[i].from / 1000.0 * kFps)));
    }
    // A start in silence moves to the sound, the words keep their times
    QVector<Transcript::Word> late = speech("Na gut, dann eben so.", 2000);
    late[0].from = 1700; // whisper starts the first word 0.3 s early, in silence
    Transcript::Levels l2;
    for (qint64 t = 0; t < 5000; t += 10) l2.db << (t >= 1950 && t < late.last().to ? -20.f : -90.f);
    const QVector<SubtitleCue> moved = Transcript::readableCues(late, l2, kFps);
    if (CHECK_EQ(moved.size(), 1)) {
        CHECK(moved[0].start > int(std::lround(late[0].from / 1000.0 * kFps)));
        if (CHECK_EQ(moved[0].words.size(), 5))
            CHECK_EQ(moved[0].start + moved[0].words[1].from, int(std::lround(late[1].from / 1000.0 * kFps)));
    }
}

void testPodcastSentences()
{
    std::printf("long sentences: balanced, no orphans\n");
    // Sentences of a real podcast transcription (whisper, German) that left "dazu gehört." / "aufnehmen?" alone;
    // words evenly spread, 330 ms each
    const QString text = "Willkommen zurück zu unserem kleinen Podcast über Videospiele und alles, was dazu gehört. "
                         "Warum macht Videoschnitt eigentlich so wenig Spaß, obwohl alle ständig Clips aufnehmen? "
                         "Ich habe letzte Woche ein Highlight von drei Kids verschnitten. "
                         "Das ist auch ein Problem. Die Programme sind entweder zu einfach oder viel zu kompliziert.";
    QVector<Transcript::Word> words;
    qint64 at = 0;
    for (const QString& w : text.split(' ')) {
        words << Transcript::Word{at, at + 330, w};
        at += 330;
    }
    const QVector<SubtitleCue> out = Subtitles::readable(Transcript::wordCues(words, kFps), kFps);
    QStringList flat;
    for (const SubtitleCue& c : out) {
        flat << QString(c.text).replace('\n', ' ');
        std::printf("       [%5.2f-%5.2f] %s\n", c.start / kFps, c.end / kFps, qPrintable(QString(c.text).replace('\n', " / ")));
    }
    CHECK_EQ(wordsOf(out), text.split(' '));
    for (int i = 0; i < out.size(); ++i) {
        const QString t = flat[i];
        const bool sentenceStart = i == 0 || flat[i - 1].endsWith('.') || flat[i - 1].endsWith('?');
        const bool whole = sentenceStart && (t.endsWith('.') || t.endsWith('?'));
        if (!whole && !CHECK(t.size() >= 20 && t.split(' ').size() > 2)) std::printf("       orphan: %s\n", qPrintable(t));
        for (const char* dangling : {" was", " so", " zu", " und", " die", " der"}) CHECK(!t.endsWith(dangling));
        for (const QString& l : out[i].text.split('\n')) CHECK(l.size() <= 42);
    }
    CHECK(flat.contains("Ich habe letzte Woche ein Highlight von drei Kids verschnitten."));
    CHECK(flat.contains("Das ist auch ein Problem."));
    CHECK(flat.contains("Die Programme sind entweder zu einfach oder viel zu kompliziert."));
    // The two long sentences: two pieces each of about equal length
    for (const char* first : {"Willkommen", "Warum"})
        for (int i = 0; i + 1 < flat.size(); ++i)
            if (flat[i].startsWith(first)) {
                std::printf("       %s: %d + %d characters\n", first, int(flat[i].size()), int(flat[i + 1].size()));
                CHECK(std::abs(flat[i].size() - flat[i + 1].size()) <= 24);
                CHECK(flat[i + 1].endsWith('.') || flat[i + 1].endsWith('?'));
            }
}

QJsonObject run(const QString& name, QJsonObject raw)
{
    if (name == "edit") raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

ProjectData load(const QString& path)
{
    ProjectData d;
    QString error;
    ProjectFile::load(path, &d, &error);
    return d;
}

void testOp(const QString& dir)
{
    std::printf("edit op subtitles\n");
    const QString path = dir + "/subs.schneidi";
    {
        Project p;
        ProjectData d;
        d.format = kFormat;
        d.timeline.video.resize(1);
        p.load(d);
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    // Words (as `transcribe` words:true lists them, timeline frames): readable cues with the word times
    const QVector<SubtitleCue> wc = Transcript::wordCues(speech(kText), kFps);
    QJsonArray words;
    for (const SubtitleCue& c : wc) words << QJsonObject{{"from", c.start}, {"to", c.end}, {"text", c.text}};
    run("edit", {{"project", path}, {"ops", QJsonArray{QJsonObject{{"op", "subtitles"}, {"words", words}, {"name", "W"}}}}});
    ProjectData d = load(path);
    if (CHECK_EQ(d.timeline.subtitles.size(), 1)) {
        CHECK(d.timeline.subtitles[0].cues == [&] {
            QVector<SubtitleCue> expect = Subtitles::readable(wc, kFps);
            for (int i = 0; i < expect.size(); ++i) expect[i].id = d.timeline.subtitles[0].cues.value(i).id;
            return expect;
        }());
        CHECK(!d.timeline.subtitles[0].cues.first().words.isEmpty());
    }
    // A cue list as given, then the track regrouped in place
    const QVector<SubtitleCue> old = Transcript::toCues(speech(kText), kFps);
    QJsonArray cues;
    for (const SubtitleCue& c : old) cues << QJsonObject{{"from", c.start}, {"to", c.end}, {"text", c.text}};
    run("edit", {{"project", path}, {"ops", QJsonArray{QJsonObject{{"op", "subtitles"}, {"cues", cues}}}}});
    d = load(path);
    if (!CHECK_EQ(d.timeline.subtitles.size(), 2)) return;
    CHECK_EQ(d.timeline.subtitles[1].cues.size(), old.size());
    const QJsonObject done = run("edit", {{"project", path}, {"ops", R"([{"op":"subtitles","track":"ST2","readable":true}])"}});
    std::printf("       %s\n", QJsonDocument(done["notes"].toArray()).toJson(QJsonDocument::Compact).constData());
    d = load(path);
    CHECK(d.timeline.subtitles[1].cues.size() < old.size());
    CHECK_EQ(fastCount(d.timeline.subtitles[1].cues), 0);
    CHECK_EQ(wordsOf(d.timeline.subtitles[1].cues), wordsOf(old));
    QSet<int> ids;
    for (const SubtitleCue& c : d.timeline.subtitles[1].cues) ids << c.id;
    CHECK(!ids.contains(0) && ids.size() == d.timeline.subtitles[1].cues.size());
    // readable with other rules on a new cue list
    run("edit", {{"project", path},
                 {"ops", QJsonArray{QJsonObject{{"op", "subtitles"}, {"cues", cues},
                                                {"readable", QJsonObject{{"lines", 1}, {"line_chars", 32}}}}}}});
    d = load(path);
    if (CHECK_EQ(d.timeline.subtitles.size(), 3))
        for (const SubtitleCue& c : d.timeline.subtitles[2].cues) CHECK(!c.text.contains('\n') && c.text.size() <= 32);
    // Mistakes are named
    bool refused = false;
    try {
        run("edit", {{"project", path}, {"ops", R"([{"op":"subtitles","track":"ST2"}])"}});
    } catch (const Cli::Error& e) {
        refused = e.code == "BAD_ARGUMENT";
    }
    CHECK(refused);
    // A track whose cues have no text: said so
    QString message;
    {
        Project p;
        ProjectData pd = load(path);
        SubtitleTrack empty;
        empty.cues = {{900001, 10, 30, " "}};
        pd.timeline.subtitles << empty;
        p.load(pd);
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    try {
        run("edit", {{"project", path}, {"ops", R"([{"op":"subtitles","track":"ST4","readable":true}])"}});
    } catch (const Cli::Error& e) {
        message = e.message;
    }
    std::printf("       %s\n", qPrintable(message));
    CHECK(message.contains("no text"));
    // Cues with their own word times (what `transcribe` readable applies) keep them
    run("edit", {{"project", path}, {"ops", R"([{"op":"subtitles","cues":[{"from":10,"to":40,"text":"zwei Worte","words":[[12,20],[25,38]]}]}])"}});
    d = load(path);
    if (CHECK_EQ(d.timeline.subtitles.size(), 5) && CHECK_EQ(d.timeline.subtitles[4].cues.size(), 1))
        CHECK(d.timeline.subtitles[4].cues[0].words == QVector<CueWord>({{2, 10}, {15, 28}}));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("subtitles_readable");
    I18n::setLanguage("en");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testFromWords();
    testFromCues();
    testPausesAndSentences();
    testOverlapsAndSound();
    testPodcastSentences();
    testOp(tmp.path());
    return Check::result();
}
