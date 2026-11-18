// SPDX-License-Identifier: AGPL-3.0-only
// Rough cut from a script and the best takes (0.17 package "rough cut"): core/ScriptAlign (normalized words, fuzzy
// alignment, takes and their rating, repeats without a script) and the edit ops rough_cut / best_takes on a project
// file whose media have transcripts (made-up word lists, no whisper).
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ScriptAlign.h"
#include "core/TimelineOps.h"

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <memory>

using namespace ScriptAlign;

namespace {

// Words of a text, 0.4 s each from t0; "|" = a pause of 1.5 s
QVector<Word> say(const QString& text, double t0)
{
    QVector<Word> w;
    double t = t0;
    for (const QString& s : text.split(' ', Qt::SkipEmptyParts)) {
        if (s == "|") {
            t += 1.5;
            continue;
        }
        w << Word{s, t, t + 0.36};
        t += 0.4;
    }
    return w;
}

void testWords()
{
    CHECK_EQ(normalize("Größe,"), QString("groesse"));
    CHECK_EQ(normalize("Zwanzig!"), QString("20"));
    CHECK_EQ(normalize("three"), QString("3"));
    CHECK_EQ(normalize("Café"), QString("cafe"));
    CHECK(isFiller(normalize("Ähm")));
    CHECK(!isFiller(normalize("Kamera")));
    CHECK(similarity("kameras", "kamera") > 0.8);
    CHECK(similarity("ich", "ist") == 0); // short words: exact only
    const QVector<Sentence> s = sentences("Heute zeige ich euch das neue Handy. Es hat drei Kameras!\n\nTschüss");
    if (CHECK_EQ(s.size(), 3)) {
        CHECK_EQ(s[0].words.size(), 7);
        CHECK_EQ(s[1].words, QStringList({"es", "hat", "3", "kameras"}));
        CHECK_EQ(s[2].text, QString("Tschüss"));
    }
}

void testTakes()
{
    const QVector<Sentence> script = sentences("Heute zeige ich euch das neue Handy. Es hat drei Kameras. Das war es.");
    // Three takes of the first sentence, the middle one with a slip; the second one misheard by whisper
    const QVector<Word> w = say("Heute zeige ich euch das neue Handy. Heute zeige ich euch das alte äh neue Händy. "
                                "Heute zeige ich euch das neue Handy. Es hat 3 Kamera.",
                                0);
    QVector<Choice> c = roughCut(script, {w}, Pick::LastGood);
    if (!CHECK_EQ(c.size(), 3)) return;
    if (CHECK_EQ(c[0].takes.size(), 3)) {
        CHECK(c[0].takes[0].good && !c[0].takes[1].good && c[0].takes[2].good);
        CHECK_EQ(c[0].takes[1].extra, 1);  // "alte"
        CHECK_EQ(c[0].takes[1].fillers, 1); // "äh"
        CHECK_EQ(c[0].chosen, 2);
        CHECK(c[0].why.contains("last complete"));
        CHECK_EQ(c[0].takes[2].first, 16);
        CHECK(std::abs(c[0].takes[2].from - 6.4) < 1e-9);
    }
    if (CHECK_EQ(c[1].takes.size(), 1)) CHECK(c[1].takes[0].good && c[1].takes[0].coverage == 1.0); // 3 = drei, Kamera
    CHECK_EQ(c[2].chosen, -1); // not said
    CHECK(c[2].why.contains("not found"));

    // best: the highest score; with only flawed takes the best of them
    const QVector<Word> flawed = say("Heute zeige ich das neue Handy. Heute zeige ich euch das | neue Handy.", 0);
    c = roughCut({script[0]}, {flawed}, Pick::LastGood);
    if (CHECK_EQ(c[0].takes.size(), 2)) {
        CHECK(!c[0].takes[0].good && !c[0].takes[1].good); // a word missing; a long pause
        CHECK(c[0].chosen >= 0 && c[0].why.contains("best"));
    }
    // A take is used once: the same sentence twice in the script takes two takes
    const QVector<Sentence> twice = sentences("Es hat drei Kameras. Es hat drei Kameras.");
    c = roughCut(twice, {say("Es hat drei Kameras. Es hat drei Kameras.", 0)}, Pick::LastGood);
    if (CHECK(c[0].chosen >= 0 && c[1].chosen >= 0)) CHECK(c[0].takes[c[0].chosen].first != c[1].takes[c[1].chosen].first);
    // Several sources: found in the second
    c = roughCut({script[1]}, {say("Hallo zusammen.", 0), say("Es hat drei Kameras.", 0)}, Pick::Best);
    if (CHECK(c[0].chosen >= 0)) CHECK_EQ(c[0].takes[c[0].chosen].source, 1);

    // Repeats without a script: a false start and its full take; a sentence said once is no repeat
    // A longer take with a slip before the right one: the right (last) one stays
    const QVector<Repeat> slip = repeats(say("Heute zeige ich euch das alte neue Handy. Heute zeige ich euch das neue Handy.", 0));
    if (CHECK_EQ(slip.size(), 1) && CHECK_EQ(slip[0].takes.size(), 2)) {
        CHECK_EQ(slip[0].keep, 1);
        CHECK_EQ(slip[0].takes[0].extra, 1);
    }
    const QVector<Repeat> r = repeats(say("Also ich wollte sagen. Also ich wollte euch sagen dass es klappt. Danke schön.", 0));
    if (CHECK_EQ(r.size(), 1) && CHECK_EQ(r[0].takes.size(), 2)) {
        CHECK_EQ(r[0].keep, 1);
        CHECK(r[0].takes[0].coverage < 0.9);
    }
    CHECK(repeats(say("Das ist gut. Das ist schlecht.", 0)).isEmpty());
}

// ---------- schneidi-cli ----------

QString g_path;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QJsonArray& ops)
{
    try {
        run("edit", {{"project", g_path}, {"ops", ops}});
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

std::unique_ptr<Project> loaded()
{
    ProjectData d;
    ProjectFile::load(g_path, &d, nullptr);
    auto p = std::make_unique<Project>();
    p->load(d);
    return p;
}

// As the app stores them: a word's end reaches up to the next word
MediaTranscript transcriptOf(const QVector<Word>& words)
{
    MediaTranscript t{0, "de", {}};
    for (int i = 0; i < words.size(); ++i) {
        const double to = i + 1 < words.size() ? words[i + 1].from : words[i].to;
        t.words << TranscriptWord{qint64(std::lround(words[i].from * 1000)), qint64(std::lround(to * 1000)), words[i].text};
    }
    return t;
}

void testOps(const QString& dir)
{
    g_path = dir + "/talk.schneidi";
    // Made-up media (nothing on disk), absolute as the ops make their paths (Windows: with the drive)
    {
        Project p;
        MediaInfo a{Check::absPath("/x/a.mp4"), "a.mp4", 25 * 60, true, true, false};
        a.transcripts << transcriptOf(say("Heute zeige ich euch das neue Handy. Heute zeige ich euch das alte äh neue "
                                          "Handy. Heute zeige ich euch das neue Handy.", 0));
        MediaInfo b{Check::absPath("/x/b.mp4"), "b.mp4", 25 * 60, true, true, false};
        b.transcripts << transcriptOf(say("Es hat drei Kameras. Das war es.", 10));
        // Two takes, the later one with a stall of 1.5 s in it: the earlier, fluent one is the last good one
        MediaInfo c{Check::absPath("/x/c.mp4"), "c.mp4", 25 * 60, true, true, false};
        c.transcripts << transcriptOf(say("Wir fahren heute ans Meer. Wir fahren heute | ans Meer.", 0));
        MediaInfo q{Check::absPath("/x/quiet.mp4"), "quiet.mp4", 25 * 60, true, true, false}; // no transcript
        p.addMedia(a);
        p.addMedia(b);
        p.addMedia(c);
        p.addMedia(q);
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        p.load(ProjectData{ProjectFormat{}, 0, p.media(), {}, tl, 0, 0});
        QString error;
        if (!CHECK(ProjectFile::save(p.data(), g_path, &error))) return;
    }
    const QJsonObject r = run("edit", {{"project", g_path},
                                       {"ops", QJsonArray{QJsonObject{{"op", "rough_cut"},
                                                                      {"script", "Es hat drei Kameras. Heute zeige ich "
                                                                                 "euch das neue Handy. Und tschüss."}}}}});
    const QString answer = QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
    CHECK(answer.contains("\"missing\":[\"Und tschüss.\"]"));
    CHECK(answer.contains("last complete"));
    {
        const auto p = loaded();
        int seq = 0;
        for (const Sequence& q : p->sequences())
            if (p->sequenceName(q.id) == "Rohschnitt") seq = q.id;
        if (CHECK(seq)) {
            p->setCurrentSequence(seq);
            const QVector<Clip>& v = p->timeline().video[0].clips;
            if (CHECK_EQ(v.size(), 2)) {
                // b.mp4 first (script order), from just before 10.0 s; then the third take of a.mp4 (word 16, 6.4 s)
                CHECK_EQ(v[0].mediaPath, Check::absPath("/x/b.mp4"));
                CHECK(std::abs(v[0].in - 248) <= 1); // 9.92 s
                CHECK_EQ(v[1].mediaPath, Check::absPath("/x/a.mp4"));
                CHECK(std::abs(v[1].in - 160) <= 1); // 6.4 s: the word before ends right there
                CHECK(v[1].start == v[0].end());
                // ends after the last word's spoken end (8.8 + 0.25 + 0.07 * 6 + 0.12 s), before the file's next word
                CHECK(v[1].out - v[1].in + 1 < 3.3 * 25);
            }
        }
    }
    {
        const QJsonObject sea = run("edit", {{"project", g_path}, {"ops", QJsonArray{QJsonObject{
                                                  {"op", "rough_cut"}, {"media", QJsonArray{Check::absPath("/x/c.mp4")}},
                                                  {"script", "Wir fahren heute ans Meer."}, {"name", "Meer"}}}}});
        const QString t = QString::fromUtf8(QJsonDocument(sea).toJson(QJsonDocument::Compact));
        if (!CHECK(t.contains("take 1 of 2: the last complete one"))) std::printf("       %s\n", qPrintable(t.left(600)));
    }
    // Errors: no script, a wrong takes value, nothing found
    CHECK(errorOf({QJsonObject{{"op", "rough_cut"}}}).startsWith("BAD_ARGUMENT"));
    CHECK(errorOf({QJsonObject{{"op", "rough_cut"}, {"script", "Hallo."}, {"takes", "first"}}}).contains("last_good"));
    {
        const QString e = errorOf({QJsonObject{{"op", "rough_cut"}, {"script", "Völlig anderer Text hier."}}});
        if (!CHECK(e.startsWith("NOT_FOUND"))) std::printf("       %s\n", qPrintable(e));
    }

    // Again: a new timeline beside the first one
    const QJsonObject again = run("edit", {{"project", g_path},
                                           {"ops", QJsonArray{QJsonObject{{"op", "rough_cut"}, {"script", "Es hat drei Kameras."}}}}});
    CHECK(QJsonDocument(again).toJson().contains("Rohschnitt 2"));

    // best_takes never cuts into a clip without words between two takes (it cut up to the next word anywhere)
    run("edit", {{"project", g_path},
                 {"ops", QJsonArray{QJsonObject{{"op", "new_timeline"}, {"name", "Zwischen"}},
                                    QJsonObject{{"op", "keep"}, {"media", Check::absPath("/x/a.mp4")}, {"ranges", QJsonArray{QJsonArray{"0s", "2.8s"}}}},
                                    QJsonObject{{"op", "keep"}, {"media", Check::absPath("/x/quiet.mp4")}, {"ranges", QJsonArray{QJsonArray{"0s", "2s"}}}},
                                    QJsonObject{{"op", "keep"}, {"media", Check::absPath("/x/a.mp4")}, {"ranges", QJsonArray{QJsonArray{"6.4s", "9.2s"}}}}}}});
    run("edit", {{"project", g_path}, {"timeline", "Zwischen"}, {"ops", QJsonArray{QJsonObject{{"op", "best_takes"}}}}});
    {
        const auto p = loaded();
        for (const Sequence& q : p->sequences())
            if (p->sequenceName(q.id) == "Zwischen") p->setCurrentSequence(q.id);
        int quiet = 0;
        for (const Clip& c : p->timeline().video[0].clips)
            if (c.mediaPath == Check::absPath("/x/quiet.mp4")) quiet += c.length();
        CHECK_EQ(quiet, 50); // all of it
    }

    // best_takes on a timeline of all of a.mp4: the first two takes go, the last one stays
    run("edit", {{"project", g_path},
                 {"ops", QJsonArray{QJsonObject{{"op", "new_timeline"}, {"name", "Alles"}},
                                    QJsonObject{{"op", "keep"}, {"media", Check::absPath("/x/a.mp4")}, {"ranges", QJsonArray{QJsonArray{"0s", "10s"}}}}}}});
    const QJsonObject b = run("edit", {{"project", g_path}, {"timeline", "Alles"},
                                       {"ops", QJsonArray{QJsonObject{{"op", "best_takes"}}}}});
    const QString bt = QString::fromUtf8(QJsonDocument(b).toJson(QJsonDocument::Compact));
    if (!CHECK(bt.contains("best_takes") && bt.contains("\"removed\""))) std::printf("       %s\n", qPrintable(bt));
    {
        const auto p = loaded();
        for (const Sequence& q : p->sequences())
            if (p->sequenceName(q.id) == "Alles") p->setCurrentSequence(q.id);
        const QVector<Clip>& v = p->timeline().video[0].clips;
        // What is left starts with the third take (source 6.4 s)
        if (CHECK(!v.isEmpty()) && !CHECK(std::abs(v[0].in - 160) <= 1))
            for (const Clip& c : v) std::printf("       clip %d start %d in %d out %d\n", c.id, c.start, c.in, c.out);
        CHECK(TimelineOps::endFrame(p->timeline()) < 6 * 25);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("rough-cut");
    QTemporaryDir dir;
    testWords();
    testTakes();
    testOps(dir.path());
    return Check::result();
}
