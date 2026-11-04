// SPDX-License-Identifier: AGPL-3.0-only
// Effect sounds (core/SfxSynth, core/SfxPlace, op 'sfx'): the synthesis is deterministic and mild (peak <= -1 dBFS),
// the placement puts the anchor of a sound on its moment and keeps the rules (spacing, cap per minute, one impact).
// SCHNEIDI_SFX_SAMPLE_DIR=<folder>: also writes the listening sample there (one WAV per candidate, all of them in
// one file with a second of silence between, LISTE.md). Never plays anything.

#include "check.h"
#include "cli/Commands.h"
#include "core/SfxPlace.h"
#include "core/SfxSynth.h"
#include "engine/Extensions.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>

namespace {

struct Candidate {
    SfxSynth::Kind kind;
    unsigned seed;
    double length; // 0 = default
};

const Candidate kCandidates[] = {
    {SfxSynth::Kind::Whoosh, 1, 0}, {SfxSynth::Kind::Whoosh, 2, 0}, {SfxSynth::Kind::Whoosh, 3, 0},
    {SfxSynth::Kind::Whoosh, 4, 0}, {SfxSynth::Kind::Whoosh, 5, 0}, {SfxSynth::Kind::Whoosh, 6, 0},
    {SfxSynth::Kind::Swish, 1, 0},  {SfxSynth::Kind::Swish, 2, 0},  {SfxSynth::Kind::Swish, 3, 0},  {SfxSynth::Kind::Riser, 1, 2.0},
    {SfxSynth::Kind::Riser, 2, 4.0}, {SfxSynth::Kind::Hit, 1, 0},   {SfxSynth::Kind::Hit, 2, 0},
    {SfxSynth::Kind::Hit, 3, 0},    {SfxSynth::Kind::Impact, 1, 0}, {SfxSynth::Kind::Impact, 2, 0},
    {SfxSynth::Kind::Click, 1, 0},  {SfxSynth::Kind::Click, 2, 0},
};

// 16-bit as the WAV holds it
double quantizedPeakDb(const std::vector<float>& s)
{
    long peak = 0;
    for (float v : s) peak = std::max(peak, std::abs(std::lround(double(v) * 32767.0)));
    return peak > 0 ? 20.0 * std::log10(double(peak) / 32767.0) : -200.0;
}

double rmsDb(const std::vector<float>& s)
{
    double sum = 0;
    for (float v : s) sum += double(v) * v;
    return 10.0 * std::log10(std::max(1e-20, sum / double(std::max<size_t>(1, s.size()))));
}

void testSynth()
{
    for (SfxSynth::Kind k : SfxSynth::kKinds) {
        const QString name = SfxSynth::kindName(k);
        const SfxSynth::Sound a = SfxSynth::make(k, 7), b = SfxSynth::make(k, 7), c = SfxSynth::make(k, 8);
        CHECK(!a.samples.empty());
        if (!CHECK(a.samples == b.samples)) std::printf("       %s: the same seed gave other samples\n", qPrintable(name));
        if (!CHECK(a.samples != c.samples)) std::printf("       %s: seed 7 and 8 sound the same\n", qPrintable(name));
        const double peak = quantizedPeakDb(a.samples);
        if (!CHECK(peak <= -1.0 && peak > -30.0)) std::printf("       %s: peak %.2f dBFS\n", qPrintable(name), peak);
        CHECK(std::abs(peak - SfxSynth::peakLevelDb(k)) < 0.1);
        // Whoosh and swish end where their tail has faded (about their default length); the others exactly
        if (k == SfxSynth::Kind::Whoosh || k == SfxSynth::Kind::Swish)
            CHECK(a.length > 0.3 && a.length < 2.5 && std::abs(a.length * SfxSynth::kRate - double(a.samples.size())) < 1);
        else
            CHECK_EQ(int(a.samples.size()), int(std::lround(SfxSynth::defaultLength(k) * SfxSynth::kRate)));
        // No click at the ends, nothing but numbers
        CHECK(std::abs(a.samples.back()) < 1e-4f);
        CHECK(std::all_of(a.samples.begin(), a.samples.end(), [](float v) { return std::isfinite(v); }));
        CHECK(a.anchor >= 0 && a.anchor <= a.length);
        CHECK(SfxSynth::kindFrom(name) == k);
    }
    // The whoosh is loudest at its anchor (peak lands on the cut); the riser grows to its end
    {
        const SfxSynth::Sound w = SfxSynth::make(SfxSynth::Kind::Whoosh, 1);
        size_t loud = 0;
        double best = 0, run = 0;
        const size_t win = SfxSynth::kRate / 50;
        for (size_t i = 0; i < w.samples.size(); ++i) {
            run += double(w.samples[i]) * w.samples[i];
            if (i >= win) run -= double(w.samples[i - win]) * w.samples[i - win];
            if (run > best) best = run, loud = i - win / 2;
        }
        const double at = double(loud) / SfxSynth::kRate;
        if (!CHECK(std::abs(at - w.anchor) < 0.1)) std::printf("       whoosh: loudest at %.3f s, anchor %.3f s\n", at, w.anchor);
        const SfxSynth::Sound r = SfxSynth::make(SfxSynth::Kind::Riser, 1, 3.0);
        CHECK(std::abs(r.length - 3.0) < 1e-9);
        CHECK(std::abs(r.anchor - 3.0) < 1e-9);
        const size_t n = r.samples.size();
        const std::vector<float> first(r.samples.begin(), r.samples.begin() + n / 4),
            last(r.samples.begin() + n * 3 / 4, r.samples.end() - SfxSynth::kRate / 100);
        CHECK(rmsDb(last) > rmsDb(first) + 12);
    }
    // Lengths are clamped to the kind's range
    CHECK(std::abs(SfxSynth::make(SfxSynth::Kind::Riser, 1, 30).length - SfxSynth::maxLength(SfxSynth::Kind::Riser)) < 1e-9);
    CHECK(std::abs(SfxSynth::make(SfxSynth::Kind::Hit, 1, 0.01).length - SfxSynth::minLength(SfxSynth::Kind::Hit)) < 1e-9);
    // The cache file: written once, a valid WAV of the sound
    const QString file = SfxSynth::cachedFile(SfxSynth::Kind::Hit, 3);
    CHECK(!file.isEmpty() && QFileInfo(file).size() == 44 + 2 * qint64(SfxSynth::make(SfxSynth::Kind::Hit, 3).samples.size()));
    CHECK_EQ(SfxSynth::cachedFile(SfxSynth::Kind::Hit, 3), file);
    CHECK(file.contains("/sfx/hit-3-500-v"));
}

void testPlace()
{
    using namespace SfxPlace;
    Rules rules;
    rules.maxPerMinute = 0;
    // The anchor lands on the moment
    Result r = place({{10.0}, {20.0}}, 0.6, 60, {}, rules);
    CHECK_EQ(int(r.placed.size()), 2);
    if (r.placed.size() == 2) {
        CHECK(std::abs(r.placed[0].start - 9.4) < 1e-9);
        CHECK(std::abs(r.placed[1].moment - 20.0) < 1e-9);
    }
    // Minimum spacing 1.5 s: 10.0 and 12.0 stay, 11.0 and 12.4 go; an existing sound counts too
    r = place({{10.0}, {11.0}, {12.0}, {12.4}, {30.0}}, 0, 60, {{29.0, false}}, rules);
    CHECK_EQ(int(r.placed.size()), 2);
    CHECK_EQ(int(r.skipped.size()), 3);
    for (const Skipped& s : r.skipped) CHECK_EQ(s.why, QString("too_close"));
    // A heavier moment wins over an earlier one nearby
    r = place({{10.0, 1}, {11.0, 5}}, 0, 60, {}, rules);
    CHECK(r.placed.size() == 1 && std::abs(r.placed[0].moment - 11.0) < 1e-9);
    // Cap per minute: 3 in any 60 s
    rules.maxPerMinute = 3;
    r = place({{1}, {5}, {9}, {13}, {17}, {70}}, 0, 120, {}, rules);
    CHECK_EQ(int(r.placed.size()), 4); // 1, 5, 9 and 70 (13, 17 over the cap)
    CHECK(r.skipped.size() == 2 && r.skipped[0].why == "per_minute");
    // One impact per video, also with one on the timeline already
    rules.maxPerMinute = 0;
    rules.impact = true;
    r = place({{5}, {40}}, 0, 60, {}, rules);
    CHECK(r.placed.size() == 1 && r.skipped.size() == 1 && r.skipped[0].why == "impact_once");
    r = place({{5}}, 0, 60, {{30, true}}, rules);
    CHECK(r.placed.isEmpty());
    // A riser that would start before 0, a moment after the end
    rules.impact = false;
    r = place({{1.0}, {65.0}}, 2.0, 60, {}, rules);
    CHECK(r.placed.isEmpty() && r.skipped.size() == 2 && r.skipped[0].why == "before_start"
          && r.skipped[1].why == "after_end");
    // A minimum spacing below 0.5 s is raised (never a burst)
    rules.minGap = 0;
    r = place({{1.0}, {1.2}}, 0, 60, {}, rules);
    CHECK_EQ(int(r.placed.size()), 1);
}

QJsonObject run(const QString& name, const QJsonObject& raw, bool quiet = false)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (!quiet) std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"failed", e.code}, {"message", e.message}};
    }
}

QJsonObject edit(const QString& project, const QJsonArray& ops, bool quiet = false)
{
    return run("edit", {{"project", project}, {"ops", ops}}, quiet);
}

QJsonObject sfxNote(const QJsonObject& r)
{
    for (const QJsonValue& v : r["notes"].toArray())
        if (v.toObject().contains("sfx")) return v["sfx"].toObject();
    return {};
}

// The clips of the audio track named SFX: [start frame, volume dB]
QVector<QPair<int, double>> sfxClips(const QString& project)
{
    const QJsonObject info = run("info", {{"project", project}});
    QVector<QPair<int, double>> out;
    for (const QJsonValue& t : info["timeline"]["tracks"].toArray())
        if (t["name"].toString() == "SFX")
            for (const QJsonValue& c : t["clips"].toArray()) out << qMakePair(c["start"].toInt(), c["volume_db"].toDouble());
    return out;
}

// Op 'sfx' on a project: whoosh peaks on the hard cuts, spacing, one impact, nothing without being asked
void testOp(const QString& dir)
{
    const QString video = Check::makeMedia(dir + "/v.mp4", {"-f", "lavfi", "-i", "testsrc=size=160x90:rate=25:duration=14",
                                                            "-c:v", "mpeg4", "-q:v", "5"});
    if (!CHECK(!video.isEmpty())) return;
    const QString project = dir + "/p.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    // Cuts at 2 s, 2.8 s, 6 s and 10 s (frames 50, 70, 150, 250)
    QJsonArray ops{QJsonObject{{"op", "place"}, {"media", video}, {"at", 0}}};
    for (const char* at : {"2s", "2.8s", "6s", "10s"}) ops << QJsonObject{{"op", "split"}, {"at", at}};
    CHECK(!edit(project, ops).contains("failed"));
    const QByteArray before = QJsonDocument(run("info", {{"project", project}})["timeline"].toObject()).toJson();
    CHECK(sfxClips(project).isEmpty());

    // Neither 'at' nor 'on': an error, nothing changed
    CHECK_EQ(edit(project, {QJsonObject{{"op", "sfx"}, {"kind", "whoosh"}}}, true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, {QJsonObject{{"op", "sfx"}, {"on", "cuts"}, {"kind", "trumpet"}}}, true)["failed"].toString(),
             QString("NOT_FOUND"));
    CHECK(QJsonDocument(run("info", {{"project", project}})["timeline"].toObject()).toJson() == before);

    // Whoosh on the cuts: 2.8 s is too close to 2 s; the peak (anchor) of each lands on its cut
    QJsonObject r = edit(project, {QJsonObject{{"op", "sfx"}, {"on", "cuts"}}});
    QJsonObject n = sfxNote(r);
    CHECK_EQ(n["kind"].toString(), QString("whoosh"));
    CHECK(n["track"].toString().startsWith("A"));
    CHECK_EQ(n["placed"].toArray().size(), 3);
    CHECK_EQ(n["skipped"].toArray().size(), 1);
    if (n["skipped"].toArray().size() == 1) {
        CHECK(std::abs(n["skipped"][0]["at_s"].toDouble() - 2.8) < 1e-6);
        CHECK_EQ(n["skipped"][0]["why"].toString(), QString("too_close"));
    }
    QVector<QPair<int, double>> clips = sfxClips(project);
    if (CHECK_EQ(clips.size(), 3)) {
        const int cuts[] = {50, 150, 250};
        for (int i = 0; i < 3; ++i) {
            const double anchor = SfxSynth::anchorOf(SfxSynth::Kind::Whoosh, unsigned(1 + i), 0);
            const int expected = int(std::lround(cuts[i] - anchor * 25));
            if (!CHECK_EQ(clips[i].first, expected)) std::printf("       whoosh %d\n", i);
            CHECK(std::abs(clips[i].second - -10.0) < 1e-6); // level mild
        }
    }
    // Again on the same cuts: every one is taken (the sounds on the SFX track count)
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"on", "cuts"}, {"kind", "hit"}}});
    CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 0);
    CHECK_EQ(sfxClips(project).size(), 3);

    // Impact at most once per video; level normal (6 per minute: mild's 3 are taken), an own volume
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", QJsonArray{"12.5s", "3.5s"}}, {"kind", "impact"}, {"level", "normal"},
                                   {"volume_db", -3}}});
    n = sfxNote(r);
    CHECK_EQ(n["placed"].toArray().size(), 1);
    if (!CHECK(std::abs(n["placed"][0]["at_s"].toDouble() - 3.5) < 0.05)) // the earlier one (on a frame)
        std::printf("       %s\n", QJsonDocument(n).toJson(QJsonDocument::Compact).constData());
    CHECK(n["skipped"].toArray().size() == 1 && n["skipped"][0]["why"].toString() == "impact_once");
    CHECK(std::abs(n["volume_db"].toDouble() - -3) < 1e-6);
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "13s"}, {"kind", "boom"}, {"level", "normal"}}});
    CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 0); // the one on the track counts
    // A mild one now: three in this minute already
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "8s"}, {"kind", "click"}}});
    CHECK(sfxNote(r)["skipped"].toArray().size() == 1 && sfxNote(r)["skipped"][0]["why"].toString() == "per_minute");
    // Hit on a marker, starting on it
    edit(project, {QJsonObject{{"op", "marker"}, {"at", "8s"}}});
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"on", "markers"}, {"seed", 2}, {"level", "normal"}}});
    n = sfxNote(r);
    CHECK_EQ(n["kind"].toString(), QString("hit"));
    CHECK_EQ(n["placed"].toArray().size(), 1);
    bool onMarker = false;
    for (const auto& [start, db] : sfxClips(project)) onMarker |= start == 200;
    CHECK(onMarker);
    // A file as the sound
    const QString own = dir + "/ding.wav";
    CHECK(SfxSynth::writeWav(own, SfxSynth::make(SfxSynth::Kind::Click, 1).samples));
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "0.5s"}, {"kind", own}, {"level", "normal"}}});
    CHECK_EQ(sfxNote(r)["kind"].toString(), QString("ding.wav"));
    // `effects` lists the sounds
    const QJsonObject fx = run("effects", {{"kind", "sfx"}});
    CHECK_EQ(fx["sounds"].toArray().size(), 6);
    CHECK(run("effects", {})["sounds"].toArray().size() >= 6);
}

// A sound pack needs its licence file: without LICENSE it is not installed (a local archive, no download)
void testPackLicense(const QString& dir)
{
    if (QStandardPaths::findExecutable("tar").isEmpty()) {
        std::printf("  no tar: pack check skipped\n");
        return;
    }
    const auto pack = [&](const QString& name, bool license) {
        const QString src = dir + "/" + name;
        QDir().mkpath(src + "/" + name);
        QFile f(src + "/" + name + "/boom.wav");
        f.open(QIODevice::WriteOnly);
        f.write("RIFF");
        f.close();
        if (license) {
            QFile l(src + "/" + name + "/LICENSE.txt");
            l.open(QIODevice::WriteOnly);
            l.write("CC0 1.0 Universal");
        }
        QProcess tar;
        tar.start("tar", {"-cf", src + ".tar", "-C", src, name});
        tar.waitForFinished();
        return src + ".tar";
    };
    const QString target = Extensions::soundPacksDir() + "/test-pack";
    const QString bad = Extensions::installSoundPack(pack("nolicense", false), target);
    CHECK(!bad.isEmpty());
    CHECK(!QFileInfo::exists(target));
    CHECK(!QFileInfo::exists(target + ".new"));
    CHECK(Extensions::installedSoundPacks().isEmpty());
    const QString good = Extensions::installSoundPack(pack("cc0", true), target);
    if (!CHECK(good.isEmpty())) std::printf("       %s\n", qPrintable(good));
    CHECK(QFileInfo::exists(target + "/cc0/LICENSE.txt"));
    CHECK_EQ(Extensions::installedSoundPacks(), QStringList{target});
    // A licence removed by hand: the pack no longer counts
    QFile::remove(target + "/cc0/LICENSE.txt");
    CHECK(Extensions::installedSoundPacks().isEmpty());
    QDir(target).removeRecursively();
}

// The listening sample for the user: every candidate alone and all in one file, with the list
void writeSample(const QString& dir)
{
    QDir().mkpath(dir);
    std::vector<float> all;
    const std::vector<float> gap(size_t(SfxSynth::kRate), 0.f);
    QFile list(dir + "/LISTE.md");
    if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QTextStream out(&list);
    out << "# Hörprobe Klänge (schneidi 0.17, Paket I)\n\n"
        << "Alle Klänge berechnet (keine Aufnahmen; `core/SfxSynth`, Whoosh/Swish: `core/SfxWhoosh`), 48 kHz mono, 16 bit. "
        << "`alle.wav` = alle Kandidaten nacheinander, je 1 s Stille dazwischen (Reihenfolge wie unten).\n\n"
        << "| # | Datei | Art | Startwert | Länge s | Spitze dBFS | RMS dBFS | Anker s | Quelle / Lizenz |\n"
        << "|---|---|---|---|---|---|---|---|---|\n";
    int n = 0;
    for (const Candidate& c : kCandidates) {
        const SfxSynth::Sound s = SfxSynth::make(c.kind, c.seed, c.length);
        const QString file = QString("%1-%2-%3-%4.wav")
                                 .arg(++n, 2, 10, QChar('0'))
                                 .arg(SfxSynth::kindName(c.kind))
                                 .arg(c.seed)
                                 .arg(std::lround(s.length * 1000));
        SfxSynth::writeWav(dir + "/" + file, s.samples);
        all.insert(all.end(), s.samples.begin(), s.samples.end());
        all.insert(all.end(), gap.begin(), gap.end());
        const bool pass = c.kind == SfxSynth::Kind::Whoosh || c.kind == SfxSynth::Kind::Swish;
        out << QString("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 |\n")
                   .arg(n)
                   .arg(file, SfxSynth::kindName(c.kind))
                   .arg(c.seed)
                   .arg(s.length, 0, 'f', 2)
                   .arg(quantizedPeakDb(s.samples), 0, 'f', 1)
                   .arg(rmsDb(s.samples), 0, 'f', 1)
                   .arg(s.anchor, 0, 'f', 3)
                   .arg(pass ? "berechnet, Port der whoosh-Maschine von earshot (Leon Lin, MIT) + Parametersätze "
                               "der forge_v2-Bank; Code AGPL/MIT"
                             : "selbst synthetisiert, AGPL");
    }
    SfxSynth::writeWav(dir + "/alle.wav", all);
    out << "\nAnker = der Punkt, der auf dem Moment landet (Whoosh: Höhepunkt auf dem Schnitt, Riser: Ende auf dem "
           "Drop, Hit/Impact/Klick: Anschlag).\n\n"
        << "## CC0-Ideen (nicht geladen, Lizenz zu prüfen)\n\n"
        << "| Paket | angenommene Lizenz | Quelle |\n|---|---|---|\n"
        << "| Kenney „Impact Sounds“ | CC0 (zu prüfen) | kenney.nl/assets (zu prüfen) |\n"
        << "| Kenney „Interface Sounds“ | CC0 (zu prüfen) | kenney.nl/assets (zu prüfen) |\n"
        << "| Kenney „Sci-Fi Sounds“ | CC0 (zu prüfen) | kenney.nl/assets (zu prüfen) |\n"
        << "| einzelne CC0-Dateien OpenGameArt/Freesound | CC0 je Datei (zu prüfen) | opengameart.org, freesound.org "
           "(zu prüfen) |\n";
    std::printf("Hörprobe: %d Kandidaten in %s\n", n, qPrintable(dir));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("sfx");
    testSynth();
    testPlace();
    QTemporaryDir tmp;
    testPackLicense(tmp.path());
    if (Check::haveFfmpeg()) testOp(tmp.path());
    else std::printf("  no ffmpeg: op test skipped\n");
    if (const QString dir = qEnvironmentVariable("SCHNEIDI_SFX_SAMPLE_DIR"); !dir.isEmpty()) writeSample(dir);
    return Check::result();
}
