// SPDX-License-Identifier: AGPL-3.0-only
// Pauses of a conversation (0.25): core/PauseCut (per-track threshold from the noise floor, common quiet of every
// track, pad, fillers while the others are quiet - bleed does not count), ScriptAlign::isFiller in the UI languages,
// and the edit op cut_pauses on two synthetic microphones: only common quiet is cut, a noisy microphone keeps its own
// threshold, ripple over all tracks, subtitles follow, markers move to the cut, TRACK_LOCKED, a `ranges` subset, fillers
// from fake word times; live in the app the measuring runs in schneidi-cli and the app
// only cuts (one undo step, the same as on the file). SCHNEIDI_PAUSE_BENCH=1 also measures the analysis of a 40 min two-track timeline.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Live.h"
#include "core/AppPaths.h"
#include "core/LiveHost.h"
#include "core/Selection.h"
#include "core/PauseCut.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ScriptAlign.h"
#include "core/TimelineOps.h"
#include "engine/AnalysisCache.h"
#include "engine/Transcriber.h"

#include "check.h"
#include "synth_sounds.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <memory>

namespace {

constexpr int kRate = 48000;
constexpr double kFps = 25;
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

std::unique_ptr<Project> loaded(const QString& path = g_path)
{
    ProjectData d;
    ProjectFile::load(path, &d, nullptr);
    auto p = std::make_unique<Project>();
    p->load(d);
    return p;
}

QJsonObject op(std::initializer_list<QPair<QString, QJsonValue>> kv)
{
    QJsonObject o;
    for (const auto& [k, v] : kv) o[k] = v;
    return o;
}

// The cut_pauses note of an edit answer
QJsonObject pauseNote(const QJsonObject& r)
{
    for (const QJsonValue& n : r["notes"].toArray())
        if (n.toObject().contains("cut_pauses")) return n["cut_pauses"].toObject();
    return {};
}

int frame(double s) { return int(std::lround(s * kFps)); }

void testCore()
{
    using namespace PauseCut;
    // Threshold: the floor (10th percentile of frames with sound) + 8 dB, between -60 and -35
    {
        std::vector<float> clean(100, float(std::pow(10.0, -70 / 20.0)));
        for (int i = 50; i < 100; ++i) clean[size_t(i)] = 0.3f;
        CHECK(std::abs(autoThresholdDb(clean) - (-60.0)) < 1e-6); // -70 + 8 = -62, held at -60
        std::vector<float> mid(100, float(std::pow(10.0, -52 / 20.0)));
        for (int i = 60; i < 100; ++i) mid[size_t(i)] = 0.3f;
        for (int i = 0; i < 10; ++i) mid[size_t(i)] = 0.f; // digital silence is left out
        CHECK(std::abs(autoThresholdDb(mid) - (-44.0)) < 0.01);
        std::vector<float> noisy(100, float(std::pow(10.0, -40 / 20.0)));
        CHECK(std::abs(autoThresholdDb(noisy) - (-35.0)) < 1e-6); // -32 held at -35 (its hiss is quiet under it)
        // Hissing above the cap: quiet in under 2 % of its frames at -35 -> floor + 4 dB
        const Threshold hiss = autoThreshold(std::vector<float>(100, float(std::pow(10.0, -33 / 20.0))));
        CHECK(hiss.raised);
        CHECK(std::abs(hiss.db - (-29.0)) < 0.01);
        CHECK(std::abs(hiss.floorDb - (-33.0)) < 0.01);
        CHECK(hiss.quietShare > 0.99);
        // ... but not a microphone that is just loud (a floor at -45 stays capped)
        CHECK(!autoThreshold(std::vector<float>(100, float(std::pow(10.0, -45 / 20.0)))).raised);
        CHECK(std::abs(autoThresholdDb(std::vector<float>(10, 0.f)) - kMaxThresholdDb) < 1e-6);
    }
    // Only where both are quiet; runs shorter than minPause stay; pad at both ends - not at the end of what was
    // measured (no speech follows: a pad would leave a sliver of silence behind the last cut, videotest 21.11.)
    {
        std::vector<bool> a(40, true), b(40, true);
        for (int i = 0; i < 10; ++i) a[size_t(i)] = false; // a speaks 0..10
        for (int i = 10; i < 15; ++i) b[size_t(i)] = false; // b answers 10..15
        for (int i = 18; i < 20; ++i) a[size_t(i)] = false; // 15..18: 3 frames quiet - too short
        // 20..40 quiet on both: 20 frames
        const QVector<Range> r = pauses({a, b}, {0, 1}, 5, 2);
        if (CHECK_EQ(r.size(), 1)) {
            CHECK_EQ(r[0].from, 22);
            CHECK_EQ(r[0].to, 40);
            CHECK(r[0].tracks == QVector<int>({0, 1}));
            CHECK(r[0].kind == Kind::Pause);
        }
        CHECK(pauses({a, b}, {0, 1}, 21, 0).isEmpty());
        // Frames without a clip on any chosen track (B-roll between two parts) are never a pause
        std::vector<bool> present(40, true);
        for (int i = 25; i < 30; ++i) present[size_t(i)] = false;
        const QVector<Range> split = pauses({a, b}, {0, 1}, 5, 2, &present);
        if (CHECK_EQ(split.size(), 2)) {
            CHECK_EQ(split[0].from, 22);
            CHECK_EQ(split[0].to, 23);
            CHECK_EQ(split[1].from, 32);
            CHECK_EQ(split[1].to, 40);
        }
        // a alone has its quiet 10..18 too
        CHECK_EQ(pauses({a}, {0}, 5, 0).size(), 2);
    }
    // A hole (no clip, shorter than minPause, a part on both sides: two segments of one recorded call placed with a
    // sliver between them, videotest 04 at 16.7 s) is missing sound: cut with the short soundless tail before it, even
    // though the whole quiet is shorter than minPause; a longer quiet beside it keeps its pad
    {
        std::vector<bool> q(40, false), present(40, true);
        for (int i = 20; i < 25; ++i) q[size_t(i)] = true; // the recording's tail without sound
        for (int i = 25; i < 31; ++i) present[size_t(i)] = false, q[size_t(i)] = true; // the gap
        const QVector<Range> r = pauses({q}, {0}, 42, 12, &present);
        if (CHECK_EQ(r.size(), 1)) {
            CHECK_EQ(r[0].from, 20);
            CHECK_EQ(r[0].to, 31);
        }
        for (int i = 5; i < 20; ++i) q[size_t(i)] = true; // 15 frames of quiet before it: 12 stay
        const QVector<Range> padded = pauses({q}, {0}, 42, 12, &present);
        if (CHECK_EQ(padded.size(), 1)) {
            CHECK_EQ(padded[0].from, 17);
            CHECK_EQ(padded[0].to, 31);
        }
        // A gap of minPause or more (B-roll, a title between two parts) stays
        std::vector<bool> broll(60, true), qb(60, true);
        for (int i = 0; i < 10; ++i) qb[size_t(i)] = false;
        for (int i = 50; i < 60; ++i) qb[size_t(i)] = false;
        for (int i = 10; i < 50; ++i) broll[size_t(i)] = false;
        CHECK(pauses({qb}, {0}, 40, 5, &broll).isEmpty());
        // Without `present` nothing is a hole
        CHECK(pauses({q}, {0}, 42, 12).isEmpty());
        CHECK(quietFrames({0.001f, 0.5f}, -40, 4) == std::vector<bool>({true, false, true, true}));
    }
    // Fillers: cut while the other track is quiet; the filler's own bleed on the other mic does not block; the other
    // speaker does; the edges snap onto the filler track's own quiet (none within 3 frames = it stays)
    {
        QVector<TranscriptCut::Word> words;
        words << TranscriptCut::Word{10, 20, 18, "ähm", 1, 0, "de"} << TranscriptCut::Word{30, 40, 38, "Hallo", 1, 0, "de"}
              << TranscriptCut::Word{50, 60, 58, "Äh,", 2, 0, "de"} << TranscriptCut::Word{70, 80, 78, "uh", 3, 1, "en"}
              << TranscriptCut::Word{90, 94, 94, "ähm", 4, 0, "de"};
        std::vector<float> own(100, 0.0005f), other(100, 0.0005f);
        for (int i = 10; i < 18; ++i) {
            own[size_t(i)] = 0.5f;
            other[size_t(i)] = 0.05f; // bleed: 20 dB under
        }
        for (int i = 50; i < 58; ++i) {
            own[size_t(i)] = 0.4f;
            other[size_t(i)] = 0.4f; // the other one talks too
        }
        for (int i = 70; i < 78; ++i) other[size_t(i)] = 0.4f; // track 1's own filler
        for (int i = 84; i < 100; ++i) own[size_t(i)] = 0.4f;  // "ähm" in the middle of a sound: no quiet edge
        const QVector<Range> f = fillers(words, {own, other}, {-50, -50}, {0, 1});
        if (CHECK_EQ(f.size(), 2)) {
            CHECK_EQ(f[0].from, 9); // the word starts loud: from the quiet frame before it
            CHECK_EQ(f[0].to, 18);
            CHECK_EQ(f[0].text, QString("ähm"));
            CHECK(f[0].kind == Kind::Filler);
            CHECK_EQ(f[1].from, 69);
            CHECK(f[1].tracks == QVector<int>({1}));
        }
        // Not measured: every filler counts
        CHECK_EQ(fillers(words, {}, {}, {0, 1}).size(), 4);
        // The transcript's language decides: "um" is a word in German
        QVector<TranscriptCut::Word> um{TranscriptCut::Word{10, 20, 18, "um", 1, 0, "de"}};
        CHECK(fillers(um, {}, {}, {0}).isEmpty());
        um[0].language = "en";
        CHECK_EQ(fillers(um, {}, {}, {0}).size(), 1);
        // Together with the pauses as their union: an overlapping filler joins the pause, nothing counts twice
        const QVector<Range> c = combined({Range{5, 15, Kind::Pause, {0, 1}, {}}}, f);
        if (CHECK_EQ(c.size(), 2)) {
            CHECK_EQ(c[0].from, 5);
            CHECK_EQ(c[0].to, 18);
            CHECK(c[0].kind == Kind::Pause);
            CHECK_EQ(c.last().from, 69);
        }
        const QVector<Range> two = combined({}, {Range{1, 4, Kind::Filler, {0}, "äh"}, Range{3, 6, Kind::Filler, {1}, "uh"}});
        if (CHECK_EQ(two.size(), 1)) CHECK(two[0].tracks == QVector<int>({0, 1}));
    }
}

void testIsFiller()
{
    using ScriptAlign::isFiller;
    using ScriptAlign::normalize;
    struct Case {
        const char* word;
        const char* lang;
    };
    const Case yes[] = {{"äh", "de"},   {"Ähm", "de"},  {"ähhhm", ""},  {"Hmmm", "de"}, {"uh", "en"},   {"um,", "en"},
                        {"Umm", "de"},  {"erm", "en"},  {"euh", "fr"},  {"Euh...", "fr"}, {"eh", "es"}, {"ehm", "it"},
                        {"eee", "pl"},  {"yyy", "pl"},  {"Yyyy", "es"}, {"ııı", "tr"},  {"э-э", "ru"},  {"Эм", "ru"},
                        {"ммм", "uk"},  {"хм", "ru"},   {"гм", "uk"},   {"еее", "ru"},  {"उम्म", "hi"}, {"えー", "ja"},
                        {"えっと", "ja"}, {"うーん", "ja"}, {"음", "ko"},    {"呃", "zh"},   {"ahn", "pt"},  {"emmm", "pt"},
                        {"ehh", "pt"},  {"ummm", ""},   {"eeeh", "it"}};
    for (const Case& c : yes)
        if (!CHECK(isFiller(normalize(QString::fromUtf8(c.word)), c.lang)))
            std::printf("       not a filler: %s (%s)\n", c.word, c.lang);
    // Words with meaning stay: "um" (German: around), "eh"/"em" (Portuguese), "e"/"y" (and), "ее" (её folded),
    // answers that mean yes (mhm, 嗯, 어), and without a language only held sounds ("äh" there could be anything)
    const Case no[] = {{"um", "de"},  {"eh", "pt"},   {"em", "pt"},   {"e", "it"},    {"y", "es"},     {"ee", "pt"},
                       {"её", "ru"},  {"mhm", "de"},  {"Mhm", "en"},  {"嗯", "zh"},   {"어", "ko"},    {"heu", "fr"},
                       {"hum", "pt"}, {"äh", ""},     {"um", ""},     {"also", "de"}, {"like", "en"},  {"ну", "ru"},
                       {"あの", "ja"}, {"Kamera", "de"}, {"a", "pt"},   {"ah", "de"},   {"oh", "en"},    {"er", "de"},
                       {"mm", "en"},  {"uhr", "de"},  {"eben", "de"}, {"hmmm-ja", "de"}};
    for (const Case& c : no)
        if (!CHECK(!isFiller(normalize(QString::fromUtf8(c.word)), c.lang)))
            std::printf("       filler but a word: %s (%s)\n", c.word, c.lang);
    // rough_cut's own list stays as it was
    CHECK(isFiller(normalize("Ähm")));
    CHECK(isFiller(normalize("mh")));
    CHECK(!isFiller(normalize("euh")));
    // What `transcribe verbatim` gives whisper as its starting text is what isFiller knows (at least two per language),
    // also the plain-ASCII one for a code page without the language's letters
    static const QRegularExpression split(QString::fromUtf8("[\\s,.、，。।]+"));
    const auto fillersIn = [](const QString& prompt, const QString& lang) {
        int n = 0;
        for (const QString& w : prompt.split(split, Qt::SkipEmptyParts))
            if (isFiller(normalize(w), lang == "auto" ? QString() : lang)) ++n;
        return n;
    };
    const auto ascii = [](const QString& t) {
        return std::all_of(t.begin(), t.end(), [](QChar c) { return c.unicode() < 128; });
    };
    for (const char* lang : {"de", "en", "fr", "es", "it", "pt", "nl", "pl", "ru", "uk", "tr", "id", "ja", "ko", "zh", "hi", "auto"}) {
        const QString prompt = Transcriber::verbatimPrompt(lang);
        if (!CHECK(fillersIn(prompt, lang) >= 2)) std::printf("       %s: too few fillers in the prompt\n", lang);
        CHECK(Transcriber::verbatimPromptFor(lang, [](const QString&) { return true; }) == prompt);
        // A code page that holds only ASCII: Latin scripts get plain sounds, the others no prompt and a note
        QString note;
        const QString plain = Transcriber::verbatimPromptFor(lang, ascii, &note);
        const bool latin = !QStringList{"ru", "uk", "ja", "ko", "zh", "hi"}.contains(lang);
        if (latin) {
            CHECK(!plain.isEmpty() && ascii(plain));
            if (!CHECK(fillersIn(plain, lang) >= 2)) std::printf("       %s: too few fillers in the ASCII prompt\n", lang);
            CHECK(note.isEmpty());
        } else {
            CHECK(plain.isEmpty());
            CHECK(!note.isEmpty());
        }
    }
    // Never a lossy prompt: a code page that holds nothing gives none
    QString note;
    CHECK(Transcriber::verbatimPromptFor("de", [](const QString&) { return false; }, &note).isEmpty());
    CHECK(!note.isEmpty());
}

// Microphone A (clean) and B (noisy hiss at about -40 dBFS): A talks 0.2-2.9, 5.0-6.2, 9.0-9.8 s, says "ähm" at
// 10.0-10.35 s and talks again 10.7-11.8 s; B answers 2.9-5.0 s; both are quiet from about 6.2 to 9.0 s
void writeMics(const QString& a, const QString& b)
{
    Synth::Builder ma(kRate, 12.0, 11), mb(kRate, 12.0, 12);
    ma.floor();
    Synth::speech(ma, 0.2, 2.7);
    Synth::speech(ma, 5.0, 1.6);
    Synth::speech(ma, 9.0, 1.0, 120);
    ma.voice(10.0, 0.35, 120, 110, Synth::vowel(1), 0.3); // "ähm"
    Synth::speech(ma, 10.7, 1.2, 120);
    mb.floor(0.01);
    Synth::speech(mb, 2.9, 2.5, 180);
    Synth::writeWav(a, ma.s, kRate);
    Synth::writeWav(b, mb.s, kRate);
}

void setupProject(const QString& path, const QString& a, const QString& b, const QString& music)
{
    QFile::remove(path);
    run("new", {{"project", path}, {"width", 320}, {"height", 180}, {"fps", 25}});
    run("edit", {{"project", path},
                 {"ops", QJsonArray{op({{"op", "place"}, {"media", a}, {"at", 0}, {"track", "A1"}}),
                                    op({{"op", "place"}, {"media", b}, {"at", 0}, {"track", "A2"}}),
                                    op({{"op", "place"}, {"media", music}, {"at", 0}, {"track", "A3"}}),
                                    op({{"op", "subtitle"}, {"text", "Welt"}, {"at", "9.2s"}, {"to", "10s"}}),
                                    op({{"op", "marker"}, {"at", "7.5s"}}), op({{"op", "marker"}, {"at", "10.5s"}})}}});
}

int trackLength(const Track& t)
{
    int n = 0;
    for (const Clip& c : t.clips) n += c.length();
    return n;
}

void testOp(const QString& dir)
{
    const QString a = dir + "/micA.wav", b = dir + "/micB.wav", music = dir + "/music.wav";
    writeMics(a, b);
    {
        Synth::Builder m(kRate, 12.0, 13);
        Synth::music(m, 0, 12.0);
        Synth::writeWav(music, m.s, kRate);
    }
    g_path = dir + "/talk.schneidi";
    setupProject(g_path, a, b, music);
    const auto before = loaded();
    const int end0 = TimelineOps::endFrame(before->timeline());
    if (!CHECK_EQ(end0, 300)) return;

    // A fixed threshold under B's hiss: B is never quiet, nothing is cut (what a single threshold would do)
    const int hits0 = AnalysisCache::hits();
    {
        const QJsonObject r = run("edit", {{"project", g_path}, {"dry_run", true},
                                           {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}, {"threshold", -50}})}}});
        CHECK_EQ(pauseNote(r)["cut"].toInt(), 0);
        CHECK(!pauseNote(r).contains("cached")); // measured: a fresh cache folder
        // It says which track is never quiet (not "a shorter min_pause")
        if (!CHECK(pauseNote(r)["note"].toString().contains("A2"))) std::printf("       %s\n", qPrintable(pauseNote(r)["note"].toString()));
    }
    CHECK_EQ(AnalysisCache::hits(), hits0);

    // Auto: each track its own threshold; only the common quiet goes (the peaks measured above come from the cache)
    const QJsonObject r = run("edit", {{"project", g_path},
                                       {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}})}}});
    const QJsonObject note = pauseNote(r);
    const QJsonArray ranges = note["ranges"].toArray();
    if (!CHECK(!ranges.isEmpty())) {
        std::printf("       %s\n", QJsonDocument(r).toJson().constData());
        return;
    }
    const QJsonArray tracks = note["tracks"].toArray();
    if (CHECK_EQ(tracks.size(), 2)) {
        CHECK(tracks[0]["threshold_db"].toDouble() < -45); // clean mic: low
        CHECK(std::abs(tracks[1]["threshold_db"].toDouble() + 35) < 0.5); // hissing mic: held at -35
    }
    // The big pause (about 6.2-9.0 s) is cut, nothing while B talks (A is quiet there) or while A talks
    bool big = false;
    int removed = 0;
    for (const QJsonValue& v : ranges) {
        const int from = v["from"].toInt(), to = v["to"].toInt();
        removed += to - from;
        CHECK(v["kind"].toString() == "pause");
        CHECK(v["tracks"].toArray() == QJsonArray({"A1", "A2"}));
        if (from < frame(8.5) && to > frame(7.0)) {
            big = true;
            CHECK(from >= frame(5.9) && to <= frame(9.0)); // pad 0.15 s before A starts again
        }
        if (!CHECK(to <= frame(3.0) || from >= frame(4.8))) std::printf("       cut %d..%d while B talks\n", from, to);
        if (!CHECK(to <= frame(9.0) || from >= frame(11.4))) std::printf("       cut %d..%d while A talks\n", from, to);
    }
    CHECK(big);
    CHECK_EQ(AnalysisCache::hits() - hits0, 2);
    CHECK_EQ(note["cached"].toInt(), 2);

    const auto after = loaded();
    const Timeline& tl = after->timeline();
    // Ripple over all tracks, also the music on A3 that was not listened to (said so); short fades at the new edges
    CHECK_EQ(TimelineOps::endFrame(tl), end0 - removed);
    for (int t = 0; t < 3; ++t)
        if (!CHECK_EQ(trackLength(tl.audio[t]), end0 - removed)) std::printf("       A%d\n", t + 1);
    CHECK(note["also_cut"].toString().contains("A3"));
    {
        const int cut = ranges[0]["from"].toInt(); // the first cut point: nothing removed before it
        int faded = 0;
        for (const Clip& c : tl.audio[2].clips) {
            if (c.end() == cut && c.fadeOut == 1) ++faded;
            if (c.start == cut && c.fadeIn == 1) ++faded;
        }
        CHECK_EQ(faded, 2);
    }
    // The marker in the pause lands on the cut, the one after it moves up; the subtitle follows
    int bigFrom = 0, before75 = 0, before105 = 0, before92 = 0;
    for (const QJsonValue& v : ranges) {
        const int from = v["from"].toInt(), to = v["to"].toInt();
        if (from < frame(7.5) && to > frame(7.5)) bigFrom = from;
        if (to <= frame(7.5)) before75 += to - from;
        if (to <= frame(10.5)) before105 += to - from;
        if (to <= frame(9.2)) before92 += to - from;
    }
    if (CHECK_EQ(tl.markers.size(), 2)) {
        CHECK_EQ(tl.markers[0], bigFrom - before75);
        CHECK_EQ(tl.markers[1], frame(10.5) - before105);
    }
    if (CHECK(!tl.subtitles.isEmpty() && !tl.subtitles[0].cues.isEmpty()))
        CHECK_EQ(tl.subtitles[0].cues[0].start, frame(9.2) - before92);

    // Again on a fresh copy: the per-track peaks come from the cache now
    setupProject(g_path, a, b, music);
    const int hits1 = AnalysisCache::hits();
    const QJsonObject again = run("edit", {{"project", g_path}, {"dry_run", true},
                                           {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}})}}});
    CHECK_EQ(AnalysisCache::hits() - hits1, 2);
    CHECK_EQ(pauseNote(again)["cached"].toInt(), 2);
    CHECK(pauseNote(again)["ranges"].toArray() == ranges);

    // Pad: none = the whole quiet goes (longer by 2 x 4 frames per pause, up to the clip edges)
    {
        const QJsonObject r0 = run("edit", {{"project", g_path}, {"dry_run", true},
                                            {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}, {"pad", 0}})}}});
        const QJsonArray r0r = pauseNote(r0)["ranges"].toArray();
        bool found = false;
        for (const QJsonValue& v : r0r)
            for (const QJsonValue& w : ranges)
                if (v["from"].toInt() == w["from"].toInt() - 4 && v["to"].toInt() == w["to"].toInt() + 4) found = true;
        CHECK(found);
    }

    // A locked track (audio, subtitles) with content after the first cut: refused, nothing changes
    for (const char* track : {"A3", "ST1"}) {
        run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "track"}, {"track", track}, {"lock", true}})}}});
        const QByteArray locked = ProjectFile::toJson(loaded()->data(), g_path);
        const QString e = errorOf({op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}})});
        if (!CHECK(e.startsWith("TRACK_LOCKED") && e.contains(track))) std::printf("       %s\n", qPrintable(e));
        CHECK(ProjectFile::toJson(loaded()->data(), g_path) == locked);
        run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "track"}, {"track", track}, {"lock", false}})}}});
    }
    // Measured for another timeline state (live: the app checks schneidi-cli's measurement): refused
    CHECK(errorOf({op({{"op", "cut_pauses"}, {"_pauses", QJsonObject{{"fingerprint", "x"}, {"ranges", QJsonArray{}}}}})})
              .startsWith("PAUSES_STALE"));

    // ranges: exactly those (what `direct apply` sends after the user left some out); nothing is measured
    {
        const int from = ranges[0]["from"].toInt(), to = ranges[0]["to"].toInt();
        const QJsonObject rr = run("edit", {{"project", g_path},
                                            {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"ranges", QJsonArray{QJsonArray{from, to}}}})}}});
        CHECK_EQ(pauseNote(rr)["cut"].toInt(), 1);
        CHECK(!pauseNote(rr).contains("tracks"));
        CHECK_EQ(TimelineOps::endFrame(loaded()->timeline()), end0 - (to - from));
        CHECK(errorOf({op({{"op", "cut_pauses"}, {"ranges", QJsonArray{QJsonArray{10, 5}}}})}).startsWith("BAD_ARGUMENT"));
    }

    // A gap between two parts on the listened track (B-roll or a title there) is no pause; the pause inside is
    const QString parts = dir + "/parts.schneidi";
    run("new", {{"project", parts}, {"width", 320}, {"height", 180}, {"fps", 25}});
    run("edit", {{"project", parts},
                 {"ops", QJsonArray{op({{"op", "place"}, {"media", a}, {"at", 0}, {"in", 0}, {"out", "2.9s"}, {"track", "A1"}}),
                                    op({{"op", "place"}, {"media", a}, {"at", "5s"}, {"in", "5s"}, {"out", "12s"}, {"track", "A1"}})}}});
    const QJsonObject pr = run("edit", {{"project", parts}, {"dry_run", true},
                                        {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1"}})}}});
    bool inner = false;
    for (const QJsonValue& v : pauseNote(pr)["ranges"].toArray()) {
        const int from = v["from"].toInt(), to = v["to"].toInt();
        if (!CHECK(to <= frame(2.9) || from >= frame(5.0))) std::printf("       cut %d..%d in the gap\n", from, to);
        if (from < frame(8.0) && to > frame(7.0)) inner = true;
    }
    CHECK(inner);
}

// Fillers from fake word times on mic A: "ähm" at 10.0 s (B quiet) goes, "äh" at 3.5 s (B talks over it) stays
void testFillers(const QString& dir)
{
    const QString a = dir + "/micA.wav", b = dir + "/micB.wav", music = dir + "/music.wav";
    g_path = dir + "/fillers.schneidi";
    setupProject(g_path, a, b, music);
    {
        ProjectData d;
        ProjectFile::load(g_path, &d, nullptr);
        for (MediaInfo& m : d.media)
            if (m.path.endsWith("micA.wav")) {
                MediaTranscript t;
                t.language = "de";
                t.words << TranscriptWord{300, 800, "Hallo"} << TranscriptWord{3500, 3800, "äh"}
                        << TranscriptWord{9100, 9900, "also"} << TranscriptWord{10000, 10400, "Ähm,"}
                        << TranscriptWord{10500, 11000, "Welt."};
                m.transcripts << t;
            }
        QString error;
        CHECK(ProjectFile::save(d, g_path, &error));
    }
    const QJsonObject r = run("edit", {{"project", g_path}, {"dry_run", true},
                                       {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}, {"fillers", true}})}}});
    int fillers = 0;
    for (const QJsonValue& v : pauseNote(r)["ranges"].toArray()) {
        if (v["kind"].toString() != "filler") continue;
        ++fillers;
        CHECK_EQ(v["text"].toString(), QString("Ähm,"));
        // Its edges on A's own quiet next to the word's times
        if (!CHECK(v["from"].toInt() >= frame(10.0) - 3 && v["from"].toInt() <= frame(10.0)))
            std::printf("       from %d\n", v["from"].toInt());
        CHECK(v["to"].toInt() >= frame(10.35) && v["to"].toInt() <= frame(10.4) + 3);
        CHECK(v["tracks"].toArray() == QJsonArray({"A1"}));
    }
    if (!CHECK_EQ(fillers, 1)) std::printf("       %s\n", QJsonDocument(r).toJson().constData());
    CHECK(pauseNote(r)["fillers"].toString().contains("verbatim"));
    // Without fillers:true only pauses
    const QJsonObject p = run("edit", {{"project", g_path}, {"dry_run", true},
                                       {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}})}}});
    for (const QJsonValue& v : pauseNote(p)["ranges"].toArray()) CHECK(v["kind"].toString() == "pause");
}

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return 0; }
    bool livePlaying() const override { return false; }
    void liveSeek(int) override {}
    void livePlay(bool) override {}
    bool liveSave(const QString& to, QString* error) override
    {
        if (!to.isEmpty()) path = to;
        return ProjectFile::save(project.data(), path, error);
    }
    void liveGenerateProxies(const QStringList&) override {}
    QJsonObject liveProxyState(const QString&) const override { return {}; }
    void liveConnectionChanged(bool, const QString&) override {}
    void liveChanged(const LiveChange&) override {}
    void liveActivity(const LiveActivity&) override {}
    bool liveOpen(const QStringList&, bool, QString*) override { return true; }
};

QJsonObject cli(const QStringList& args, const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment())
{
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && !t.hasExpired(120000)) {
        p.waitForFinished(5);
        QCoreApplication::processEvents();
    }
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(1000);
    }
    QCoreApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (!o["ok"].toBool()) std::printf("       %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

QByteArray state(const Project& p)
{
    ProjectData d = p.data();
    d.lastClipId = d.lastLinkId = d.lastSequenceId = 0;
    return ProjectFile::toJson(d, "/x/film.schneidi");
}

// Live: schneidi-cli measures (preparePauses, `_pauses`), the app only cuts - as one undo step, the same as on the file
void testLive(const QString& dir)
{
    const QString a = dir + "/micA.wav", b = dir + "/micB.wav", music = dir + "/music.wav";
    FakeApp fake;
    fake.path = dir + "/live.schneidi";
    setupProject(fake.path, a, b, music);
    const QString copy = dir + "/live-copy.schneidi";
    QFile::remove(copy);
    CHECK(QFile::copy(fake.path, copy));
    {
        ProjectData d;
        ProjectFile::load(fake.path, &d, nullptr);
        fake.project.load(d);
        fake.project.markSaved();
    }
    QString error;
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return;
    const QString ops = R"([{"op":"cut_pauses","tracks":"A1,A2"}])";
    const QByteArray before = state(fake.project);
    const int index = fake.project.undoStack()->index();
    const int hits = AnalysisCache::hits();
    const QJsonObject live = cli({"edit", fake.path, "--ops", ops});
    CHECK(live["live"].toBool());
    CHECK_EQ(fake.project.undoStack()->index(), index + 1);
    // The app did not look at the sound itself (it would have found schneidi-cli's peaks in the shared cache)
    CHECK_EQ(AnalysisCache::hits(), hits);
    QProcessEnvironment noLive = QProcessEnvironment::systemEnvironment();
    noLive.insert("XDG_DATA_HOME", dir + "/other"); // no registry: no running app
    const QJsonObject file = cli({"edit", copy, "--ops", ops}, noLive);
    CHECK(file["ok"].toBool() && !file["live"].toBool());
    ProjectData d;
    ProjectFile::load(copy, &d, nullptr);
    Project onFile;
    onFile.load(d);
    CHECK(state(onFile) == state(fake.project));
    CHECK(state(fake.project) != before);
    fake.project.undoStack()->undo();
    CHECK(state(fake.project) == before);
    // Not the first op: schneidi-cli does not measure it, and the app never does - a clear error, nothing changes
    const QJsonObject late = cli({"edit", fake.path, "--ops", R"([{"op":"marker","at":"1s"},{"op":"cut_pauses","tracks":"A1,A2"}])"});
    CHECK_EQ(late["error"]["code"].toString(), QString("LIVE_NOT_PREPARED"));
    CHECK(state(fake.project) == before);
}

// 40 min, two microphones (a 60 s take of each placed 40 times): the analysis cold and from the cache
void benchLong(const QString& dir)
{
    if (qEnvironmentVariableIntValue("SCHNEIDI_PAUSE_BENCH") != 1) return;
    const QString a = dir + "/longA.wav", b = dir + "/longB.wav";
    {
        Synth::Builder ma(kRate, 60.0, 21), mb(kRate, 60.0, 22);
        ma.floor();
        mb.floor(0.01);
        for (double t = 0; t < 60; t += 10) {
            Synth::speech(ma, t, 4.0);
            Synth::speech(mb, t + 4.2, 3.5, 180);
        }
        Synth::writeWav(a, ma.s, kRate);
        Synth::writeWav(b, mb.s, kRate);
    }
    const QString path = dir + "/long.schneidi";
    run("new", {{"project", path}, {"width", 320}, {"height", 180}, {"fps", 25}});
    QJsonArray ops;
    for (int i = 0; i < 40; ++i) {
        ops << op({{"op", "place"}, {"media", a}, {"at", QString("%1s").arg(i * 60)}, {"track", "A1"}});
        ops << op({{"op", "place"}, {"media", b}, {"at", QString("%1s").arg(i * 60)}, {"track", "A2"}});
    }
    run("edit", {{"project", path}, {"ops", ops}});
    for (int pass = 0; pass < 2; ++pass) {
        QElapsedTimer t;
        t.start();
        const QJsonObject r = run("edit", {{"project", path}, {"dry_run", true},
                                           {"ops", QJsonArray{op({{"op", "cut_pauses"}, {"tracks", "A1,A2"}})}}});
        std::printf("bench: 40 min two tracks, %s: %lld ms, %d pauses\n", pass == 0 ? "cold" : "warm",
                    qint64(t.elapsed()), pauseNote(r)["cut"].toInt());
    }
}

} // namespace

int main(int argc, char** argv)
{
    // schneidi-cli (live part) must see the same folders: own data/cache folders for both
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", (dir.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (dir.path() + "/cache").toUtf8());
    Check::initEnv();
    QGuiApplication app(argc, argv);
    AppPaths::setProgramNames();
    testCore();
    testIsFiller();
    testOp(dir.path());
    testFillers(dir.path());
    testLive(dir.path());
    benchLong(dir.path());
    return Check::result();
}
