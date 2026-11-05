// SPDX-License-Identifier: AGPL-3.0-only
// Effect sounds (core/SfxSynth, core/SfxPlace, op 'sfx'): the synthesis is deterministic and mild (peak <= -1 dBFS),
// the placement puts the anchor of a sound on its moment and keeps the rules (spacing, cap per minute, one impact).
// SCHNEIDI_SFX_SAMPLE_DIR=<folder>: also writes the listening sample there (one WAV per candidate, all of them in
// one file with a second of silence between, LISTE.md). Never plays anything.

#include "check.h"
#include "cli/Commands.h"
#include "core/ProjectFile.h"
#include "core/SfxForge.h"
#include "core/SfxPlace.h"
#include "core/SfxSynth.h"
#include "engine/Extensions.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <cstring>

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
    {SfxSynth::Kind::Swish, 1, 0},  {SfxSynth::Kind::Swish, 2, 0},  {SfxSynth::Kind::Swish, 3, 0},
    {SfxSynth::Kind::Riser, 1, 0},  {SfxSynth::Kind::Riser, 2, 0},  {SfxSynth::Kind::Riser, 3, 0},
    {SfxSynth::Kind::Riser, 1, 2.0},
    {SfxSynth::Kind::Hit, 1, 0},    {SfxSynth::Kind::Hit, 2, 0},    {SfxSynth::Kind::Hit, 3, 0},
    {SfxSynth::Kind::Impact, 1, 0}, {SfxSynth::Kind::Impact, 2, 0}, {SfxSynth::Kind::Impact, 3, 0},
    {SfxSynth::Kind::Click, 1, 0},  {SfxSynth::Kind::Click, 2, 0},  {SfxSynth::Kind::Click, 3, 0},
};

// 16-bit as the WAV holds it (its dither adds at most 2 steps)
double quantizedPeakDb(const std::vector<float>& s)
{
    long peak = 0;
    for (float v : s) peak = std::max(peak, std::abs(std::lround(double(v) * 32767.0)) + 2);
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
        // Every sound ends where its tail has faded (about its default length)
        CHECK(a.length > 0.03 && a.length < 3.0 && std::abs(a.length * SfxSynth::kRate - double(a.samples.size())) < 1);
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
        CHECK(std::abs(r.length - 3.0) < 1e-9); // the build to the peak, cut there
        CHECK(std::abs(r.anchor - 3.0) < 1e-9);
        const size_t peak = size_t(3.0 * SfxSynth::kRate);
        const std::vector<float> first(r.samples.begin(), r.samples.begin() + peak / 4),
            last(r.samples.begin() + peak * 3 / 4, r.samples.begin() + peak);
        CHECK(rmsDb(last) > rmsDb(first) + 12);
    }
    // Lengths are clamped to the kind's range
    CHECK(std::abs(SfxSynth::make(SfxSynth::Kind::Riser, 1, 30).anchor - SfxSynth::maxLength(SfxSynth::Kind::Riser)) < 1e-9);
    CHECK(std::abs(SfxSynth::make(SfxSynth::Kind::Riser, 1, 0.01).anchor - SfxSynth::minLength(SfxSynth::Kind::Riser)) < 1e-9);
    // Without the recorded sounds (data folder missing): computed ones, cached as WAV files named by kind, seed, length
    {
        const QByteArray keep = qgetenv("SCHNEIDI_SFX_DATA");
        qputenv("SCHNEIDI_SFX_DATA", "/nonexistent/sfx");
        CHECK(SfxSynth::recorded(SfxSynth::Kind::Hit).isEmpty());
        const QString file = SfxSynth::cachedFile(SfxSynth::Kind::Hit, 3);
        const SfxSynth::Sound hit3 = SfxSynth::make(SfxSynth::Kind::Hit, 3);
        CHECK(!file.isEmpty() && QFileInfo(file).size() == 44 + 2 * qint64(hit3.samples.size()));
        CHECK_EQ(SfxSynth::cachedFile(SfxSynth::Kind::Hit, 3), file);
        CHECK(file.contains(QString("/sfx/hit-3-%1ms-v").arg(std::lround(hit3.length * 1000))));
        CHECK(std::abs(SfxSynth::anchorOf(SfxSynth::Kind::Hit, 3) - hit3.anchor) < 1e-9);
        // A whoosh's name holds its real length
        const QString w = SfxSynth::cachedFile(SfxSynth::Kind::Whoosh, 2);
        const SfxSynth::Sound ws = SfxSynth::make(SfxSynth::Kind::Whoosh, 2);
        CHECK(w.contains(QString("/sfx/whoosh-2-%1ms-v").arg(std::lround(ws.length * 1000))));
        if (keep.isEmpty()) qunsetenv("SCHNEIDI_SFX_DATA");
        else qputenv("SCHNEIDI_SFX_DATA", keep);
    }
    // The name tells kind, seed and the asked length back
    const auto parsed = SfxSynth::parseCachedFile(SfxSynth::cachedFile(SfxSynth::Kind::Riser, 2, 3.0));
    CHECK(parsed && parsed->kind == SfxSynth::Kind::Riser && parsed->seed == 2 && std::abs(parsed->asked - 3.0) < 1e-9
          && std::abs(parsed->length - 3.0) < 1e-3);
    // The dark riser (seed 3) still rings a little after its peak
    CHECK(SfxSynth::make(SfxSynth::Kind::Riser, 3, 1.0).length > 1.0);
    CHECK(!SfxSynth::parseCachedFile("/x/ding.wav"));
}

// The designed risers (SfxForge: earshot's riser build): deterministic, rising, loudest at their end (they stop on the
// peak, nothing rings after it), clean
void testForgeRisers()
{
    // 10 ms RMS envelope in dB
    const auto envelope = [](const std::vector<float>& s) {
        const size_t h = SfxSynth::kRate / 100;
        std::vector<double> e(s.size() / h);
        for (size_t b = 0; b < e.size(); ++b) e[b] = rmsDb(std::vector<float>(s.begin() + long(b * h), s.begin() + long((b + 1) * h)));
        return e;
    };
    struct Case {
        SfxForge::Riser member;
        double length;
    };
    const Case cases[] = {{SfxForge::Riser::Tension, 0.3}, {SfxForge::Riser::Tension, 2.0}, {SfxForge::Riser::Tension, 8.0},
                          {SfxForge::Riser::Swell, 0.3},   {SfxForge::Riser::Swell, 2.0},   {SfxForge::Riser::Swell, 8.0},
                          {SfxForge::Riser::Air, 2.0}};
    for (const Case& c : cases) {
        const SfxForge::Result a = SfxForge::riser(c.member, 3, c.length);
        const auto& s = a.samples;
        const QString what = QString("riser %1, %2 s").arg(int(c.member)).arg(c.length);
        if (c.length < 2.5) { // the same seed and length: the same samples; another seed: others
            CHECK(s == SfxForge::riser(c.member, 3, c.length).samples);
            CHECK(s != SfxForge::riser(c.member, 4, c.length).samples);
        }
        // The length is the build to the peak, the anchor its end
        CHECK_EQ(s.size(), size_t(std::lround(c.length * SfxSynth::kRate)));
        CHECK(std::abs(a.anchor - c.length) < 1e-9);
        CHECK(std::all_of(s.begin(), s.end(), [](float v) { return std::isfinite(v); }));
        double mean = 0;
        float peak = 0;
        for (float v : s) mean += v, peak = std::max(peak, std::abs(v));
        mean /= double(s.size());
        if (!CHECK(std::abs(mean) < 1e-3)) std::printf("       %s: mean %.2e\n", qPrintable(what), mean);
        CHECK(std::abs(peak - 1.f) < 1e-4f);
        // The last 50 ms hold the loudest 10 ms (within 1.5 dB: the noise, the last tremolo pulses)
        const std::vector<double> e = envelope(s);
        const double best = *std::max_element(e.begin(), e.end()), end = *std::max_element(e.end() - 5, e.end());
        if (!CHECK(end > best - 1.5)) std::printf("       %s: the last 50 ms %.1f dB under the loudest\n", qPrintable(what), best - end);
        // A rising envelope: the last quarter clearly louder than the first
        const size_t q = s.size() / 4;
        const double rise = rmsDb(std::vector<float>(s.end() - long(q), s.end())) - rmsDb(std::vector<float>(s.begin(), s.begin() + long(q)));
        if (!CHECK(rise > 10.0)) std::printf("       %s: rises %.1f dB\n", qPrintable(what), rise);
    }
    // Length 0: the member's own build
    CHECK(std::abs(SfxForge::riser(SfxForge::Riser::Swell, 1).anchor - SfxForge::riserLength(SfxForge::Riser::Swell)) < 1e-9);
}

// The shipped CC0 recordings: there with their licences, picked by seed, anchors from their table
// The riser without a length: the default build length of the kind (what `effects` says), every variant
void testRiserDefault()
{
    for (unsigned seed : {1u, 2u, 3u}) {
        const SfxSynth::Sound r = SfxSynth::make(SfxSynth::Kind::Riser, seed);
        if (!CHECK(std::abs(r.anchor - SfxSynth::defaultLength(SfxSynth::Kind::Riser)) < 0.05))
            std::printf("       riser %u: peak at %.2f s, default %.2f s\n", seed, r.anchor,
                        SfxSynth::defaultLength(SfxSynth::Kind::Riser));
    }
}

void testRecorded()
{
    const QString root = SfxSynth::recordedRoot();
    for (const char* f : {"/sounds.json", "/SOURCES.md", "/LICENSE-opengameart-swishes.txt", "/LICENSE-kenney-impact-sounds.txt",
                          "/LICENSE-kenney-starter-kits.txt", "/LICENSE-freesound-ffeller-impact-big.md",
                          "/LICENSE-freesound-dorian-mastin-impact-small.md"})
        CHECK(QFileInfo::exists(root + f));
    for (SfxSynth::Kind k : {SfxSynth::Kind::Whoosh, SfxSynth::Kind::Swish, SfxSynth::Kind::Hit, SfxSynth::Kind::Impact,
                             SfxSynth::Kind::Click}) {
        const QVector<SfxSynth::Recorded> list = SfxSynth::recorded(k);
        if (!CHECK(list.size() >= 3)) continue;
        for (const SfxSynth::Recorded& r : list) {
            CHECK(QFileInfo::exists(root + "/" + r.file));
            CHECK(QFileInfo::exists(root + "/" + r.licenceFile));
            CHECK_EQ(r.licence, QString("CC0-1.0"));
            CHECK(r.anchor >= 0 && r.anchor < r.length);
        }
        // Seed 1, 2 … take the files in turn, the same every time
        CHECK(SfxSynth::recordedFor(k, 1)->file == list[0].file && SfxSynth::recordedFor(k, 2)->file == list[1].file);
        CHECK(SfxSynth::recordedFor(k, unsigned(list.size()) + 1)->file == list[0].file);
        CHECK(std::abs(SfxSynth::anchorOf(k, 2) - list[1].anchor) < 1e-9);
        CHECK(std::abs(SfxSynth::soundLength(k, 2) - list[1].length) < 1e-9);
        CHECK(std::abs(SfxSynth::levelGainDb(k, 2) - (SfxSynth::peakLevelDb(k) - list[1].peakDb)) < 1e-9);
        // A copy in the cache, the file itself (kept as it is)
        const QString placed = SfxSynth::cachedFile(k, 2);
        QFile a(placed), b(root + "/" + list[1].file);
        CHECK(a.open(QIODevice::ReadOnly) && b.open(QIODevice::ReadOnly) && a.readAll() == b.readAll());
        CHECK(SfxSynth::recordedOfFile(placed) && SfxSynth::recordedOfFile(placed)->file == list[1].file);
        // A length given: computed (a recording does not stretch)
        CHECK(SfxSynth::parseCachedFile(SfxSynth::cachedFile(k, 2, 0.5 * SfxSynth::maxLength(k) + 0.5 * SfxSynth::minLength(k))));
    }
    // The riser: computed (no CC0 riser recording)
    CHECK(SfxSynth::recorded(SfxSynth::Kind::Riser).isEmpty());
    // Every recording of the table is a file with its licence file (no entry silently dropped)
    QFile table(root + "/sounds.json");
    if (CHECK(table.open(QIODevice::ReadOnly))) {
        const QJsonArray sounds = QJsonDocument::fromJson(table.readAll())["sounds"].toArray();
        CHECK(sounds.size() >= 19);
        for (const QJsonValue& v : sounds) {
            CHECK(QFileInfo::exists(root + "/" + v["file"].toString()));
            CHECK(QFileInfo::exists(root + "/" + v["licence_file"].toString()));
        }
    }
    // The whole collection stays small (it is in every package)
    qint64 bytes = 0;
    QDirIterator all(root, QDir::Files, QDirIterator::Subdirectories);
    while (all.hasNext()) bytes += QFileInfo(all.next()).size();
    CHECK(bytes < 3 * 1024 * 1024);
}

void testPlace()
{
    using namespace SfxPlace;
    Rules rules;
    rules.maxPerMinute = 0;
    // The anchor lands on the moment
    Result r = place({{10.0}, {20.0}}, 0.6, 0, 60, {}, rules);
    CHECK_EQ(int(r.placed.size()), 2);
    if (r.placed.size() == 2) {
        CHECK(std::abs(r.placed[0].start - 9.4) < 1e-9);
        CHECK(std::abs(r.placed[1].moment - 20.0) < 1e-9);
    }
    // Minimum spacing 1.5 s: 10.0 and 12.0 stay, 11.0 and 12.4 go; an existing sound counts too
    r = place({{10.0}, {11.0}, {12.0}, {12.4}, {30.0}}, 0, 0, 60, {{29.0, false}}, rules);
    CHECK_EQ(int(r.placed.size()), 2);
    CHECK_EQ(int(r.skipped.size()), 3);
    for (const Skipped& s : r.skipped) CHECK_EQ(s.why, QString("too_close"));
    // A heavier moment wins over an earlier one nearby
    r = place({{10.0, 1}, {11.0, 5}}, 0, 0, 60, {}, rules);
    CHECK(r.placed.size() == 1 && std::abs(r.placed[0].moment - 11.0) < 1e-9);
    // Cap per minute: 3 in any 60 s
    rules.maxPerMinute = 3;
    r = place({{1}, {5}, {9}, {13}, {17}, {70}}, 0, 0, 120, {}, rules);
    CHECK_EQ(int(r.placed.size()), 4); // spread: 1, 70, 17 and 9 (5 and 13 over the cap), not the first three in a row
    CHECK(r.skipped.size() == 2 && r.skipped[0].why == "per_minute" && std::abs(r.skipped[0].moment - 5) < 1e-9
          && std::abs(r.skipped[1].moment - 13) < 1e-9);
    // Ten cuts in 50 s at mild's cap: three, over the whole time (the first at 0 s, the others late and in between)
    {
        QVector<Moment> cuts;
        for (int i = 0; i < 10; ++i) cuts << Moment{5.0 * i};
        const Result c = place(cuts, 0, 0, 60, {}, rules);
        CHECK(c.placed.size() == 3 && c.placed[0].moment < 1 && c.placed[2].moment > 40);
    }
    // Sounds of another kind on the track keep their distance but not this kind's cap (hits before whooshes in a short)
    r = place({{30}}, 0, 0, 60, {{10, false, false}, {20, false, false}, {40, false, false}}, rules);
    CHECK_EQ(int(r.placed.size()), 1);
    r = place({{30}}, 0, 0, 60, {{10, false, true}, {20, false, true}, {40, false, true}}, rules);
    CHECK(r.placed.isEmpty() && r.skipped.size() == 1 && r.skipped[0].why == "per_minute");
    // An impact on the moment a riser leads into belongs to it (build and landing); next to another sound it does not
    {
        Existing riser{30, false, false};
        riser.riser = true;
        riser.sameTrack = true;
        riser.start = 28;
        riser.end = 30;
        Rules impact = rules;
        impact.impact = true;
        r = place({{30.0}}, 0.0001, 1.0, 60, {riser}, impact);
        CHECK_EQ(int(r.placed.size()), 1);
        Existing whoosh{30, false, false};
        r = place({{30.0}}, 0.0001, 1.0, 60, {whoosh}, impact);
        CHECK(r.placed.isEmpty() && r.skipped.size() == 1 && r.skipped[0].why == "too_close");
        Rules second = rules;
        second.riser = true; // a second riser on it is too close
        r = place({{30.0}}, 2.0, 2.0, 60, {riser}, second);
        CHECK(r.placed.isEmpty());
    }
    // A sound whose tail would run past the end of the timeline (black frames in a render)
    r = place({{58.0}}, 0.1, 2.5, 60, {}, rules);
    CHECK(r.placed.isEmpty() && r.skipped.size() == 1 && r.skipped[0].why == "after_end");
    // One impact per video, also with one on the timeline already
    rules.maxPerMinute = 0;
    rules.impact = true;
    r = place({{5}, {40}}, 0, 0, 60, {}, rules);
    CHECK(r.placed.size() == 1 && r.skipped.size() == 1 && r.skipped[0].why == "impact_once");
    r = place({{5}}, 0, 0, 60, {{30, true}}, rules);
    CHECK(r.placed.isEmpty());
    // A riser that would start before 0, a moment after the end
    rules.impact = false;
    r = place({{1.0}, {65.0}}, 2.0, 0, 60, {}, rules);
    CHECK(r.placed.isEmpty() && r.skipped.size() == 2 && r.skipped[0].why == "before_start"
          && r.skipped[1].why == "after_end");
    // short sfx:'mild': a hit (peak -6 dBFS) 14 dB under a game sound peaking at -3 dBFS = clip volume -11 dB; quiet
    // game sound does not lift it over 0 dB; without game sound level mild
    CHECK(std::abs(underPeakDb(-3.0, -6.0) - -11.0) < 1e-9);
    CHECK(std::abs(underPeakDb(-1.0, -20.0) - 0.0) < 1e-9);
    CHECK(std::abs(underPeakDb(std::nullopt, -6.0) - levelDb("mild")) < 1e-9);
    // A minimum spacing below 0.5 s is raised (never a burst)
    rules.minGap = 0;
    r = place({{1.0}, {1.2}}, 0, 0, 60, {}, rules);
    CHECK_EQ(int(r.placed.size()), 1);
    // Long sounds (4 s risers, 3 s apart): the stronger moment keeps its sound, the weaker one overlaps it; the
    // skipped one takes nothing from its neighbours (a third moment 2 s after it is placed)
    rules = Rules{};
    rules.maxPerMinute = 0;
    r = place({{10, 0.3}, {13, 0.9}, {19, 0.3}}, 4.0, 4.2, 60, {}, rules);
    if (CHECK_EQ(int(r.placed.size()), 2)) {
        CHECK(std::abs(r.placed[0].moment - 13) < 1e-9);
        CHECK(std::abs(r.placed[1].moment - 19) < 1e-9);
    }
    CHECK(r.skipped.size() == 1 && std::abs(r.skipped[0].moment - 10) < 1e-9 && r.skipped[0].why == "overlaps");
    // A sound on the target track blocks its time; one on another track only counts for the spacing
    Existing other{30, false, true};
    other.start = 26;
    other.end = 31;
    r = place({{33}}, 4.0, 4.2, 60, {other}, rules);
    CHECK_EQ(int(r.placed.size()), 1);
    other.sameTrack = true;
    r = place({{33}}, 4.0, 4.2, 60, {other}, rules);
    CHECK(r.placed.isEmpty() && r.skipped.size() == 1 && r.skipped[0].why == "overlaps");
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

// Kills (the kill feed's hit_s, given as the op's _kills like the live path hands them in) with their weights: the
// strongest of two close ones keeps its sound; dry_run shows what would happen and leaves the project file alone
void testKillsDryRun(const QString& dir)
{
    const QString video = Check::makeMedia(dir + "/k.mp4", {"-f", "lavfi", "-i", "testsrc=size=160x90:rate=25:duration=14",
                                                            "-c:v", "mpeg4", "-q:v", "5"});
    if (!CHECK(!video.isEmpty())) return;
    const QString project = dir + "/kills.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    CHECK(!edit(project, {QJsonObject{{"op", "place"}, {"media", video}, {"at", 0}}}).contains("failed"));
    QFile f(project);
    const auto bytes = [&] {
        f.open(QIODevice::ReadOnly);
        const QByteArray b = f.readAll();
        f.close();
        return b;
    };
    const QJsonArray kills{QJsonArray{100, 1}, QJsonArray{110, 5}, QJsonArray{300, 2}};
    const QByteArray before = bytes();
    QJsonObject r = run("edit", {{"project", project}, {"dry_run", true},
                                 {"ops", QJsonArray{QJsonObject{{"op", "sfx"}, {"on", "kills"}, {"_kills", kills}}}}});
    CHECK(!r.contains("failed"));
    CHECK(bytes() == before); // nothing written
    CHECK(sfxClips(project).isEmpty());
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"on", "kills"}, {"_kills", kills}}});
    const QJsonObject n = sfxNote(r);
    CHECK_EQ(n["kind"].toString(), QString("hit"));
    if (CHECK_EQ(n["placed"].toArray().size(), 2)) {
        CHECK(std::abs(n["placed"][0]["at_s"].toDouble() - 4.4) < 1e-6); // the strong one, not the first
        CHECK(std::abs(n["placed"][1]["at_s"].toDouble() - 12.0) < 1e-6);
    }
    CHECK(n["skipped"].toArray().size() == 1 && n["skipped"][0]["why"].toString() == "too_close");
    // The hits start on their moments (a flam may come a few ms before)
    const QVector<QPair<int, double>> clips = sfxClips(project);
    if (CHECK_EQ(clips.size(), 2)) CHECK(clips[0].first >= 107 && clips[0].first <= 110 && clips[1].first >= 297 && clips[1].first <= 300);
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
            // level mild, the recording brought to its kind's peak level
            if (!CHECK(std::abs(clips[i].second - (-10.0 + SfxSynth::levelGainDb(SfxSynth::Kind::Whoosh, unsigned(1 + i)))) < 1e-6)) std::printf("       got %f want %f\n", clips[i].second, -10.0 + SfxSynth::levelGainDb(SfxSynth::Kind::Whoosh, unsigned(1 + i)));
        }
    }
    // Again on the same cuts: every one is taken (the sounds on the SFX track count)
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"on", "cuts"}, {"kind", "hit"}}});
    CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 0);
    CHECK_EQ(sfxClips(project).size(), 3);

    // Impact at most once per video; level normal (6 per minute: mild's 3 are taken), an own volume
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", QJsonArray{"12s", "3.5s"}}, {"kind", "impact"}, {"level", "normal"},
                                   {"volume_db", -3}}});
    n = sfxNote(r);
    CHECK_EQ(n["placed"].toArray().size(), 1);
    if (!CHECK(std::abs(n["placed"][0]["at_s"].toDouble() - 3.5) < 0.05)) // the earlier one (on a frame)
        std::printf("       %s\n", QJsonDocument(n).toJson(QJsonDocument::Compact).constData());
    CHECK(n["skipped"].toArray().size() == 1 && n["skipped"][0]["why"].toString() == "impact_once");
    CHECK(std::abs(n["volume_db"].toDouble() - -3) < 1e-6);
    // The answer names the clip's own volume (the level plus the sound's evening-out), the one the timeline shows
    {
        const int at = int(std::lround(n["placed"][0]["at_s"].toDouble() * 25));
        bool found = false;
        for (const auto& [start, db] : sfxClips(project))
            if (std::abs(start - at) < 25 && std::abs(db - n["placed"][0]["clip_db"].toDouble()) < 0.01) found = true;
        CHECK(n["placed"][0].toObject().contains("clip_db") && found);
    }
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "13s"}, {"kind", "boom"}, {"level", "normal"}}});
    CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 0); // the one on the track counts
    // ... also when the second one should go to another track (one impact per video, not per track)
    edit(project, {QJsonObject{{"op", "add_track"}, {"kind", "audio"}}});
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "11s"}, {"kind", "impact"}, {"level", "normal"}, {"track", "A3"}}});
    if (!CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 0))
        std::printf("       %s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
    CHECK(sfxNote(r)["skipped"].toArray().size() == 1 && sfxNote(r)["skipped"][0]["why"].toString() == "impact_once");
    // A mild whoosh now: three whooshes in this minute already; a click has its own budget
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "8s"}, {"kind", "whoosh"}}});
    CHECK(sfxNote(r)["skipped"].toArray().size() == 1 && sfxNote(r)["skipped"][0]["why"].toString() == "per_minute");
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "12s"}, {"kind", "click"}}});
    CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 1);
    // Hit on a marker, starting on it
    edit(project, {QJsonObject{{"op", "marker"}, {"at", "8s"}}});
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"on", "markers"}, {"seed", 2}, {"level", "normal"}}});
    n = sfxNote(r);
    CHECK_EQ(n["kind"].toString(), QString("hit"));
    CHECK_EQ(n["placed"].toArray().size(), 1);
    bool onMarker = false;
    // (its main contact on the marker: a flam may come a few ms before it)
    for (const auto& [start, db] : sfxClips(project)) onMarker |= start >= 197 && start <= 200;
    CHECK(onMarker);
    // A file as the sound
    const QString own = dir + "/ding.wav";
    CHECK(SfxSynth::writeWav(own, SfxSynth::make(SfxSynth::Kind::Click, 1).samples));
    r = edit(project, {QJsonObject{{"op", "sfx"}, {"at", "0.5s"}, {"kind", own}, {"level", "normal"}}});
    CHECK_EQ(sfxNote(r)["kind"].toString(), QString("ding.wav"));
    // A video file as the sound: only its sound is placed, the picture stays; a file without sound is refused
    {
        const QString clip = Check::makeMedia(dir + "/boom.mp4", {"-f", "lavfi", "-i", "testsrc=size=160x90:rate=25:duration=1",
                                                                  "-f", "lavfi", "-i", "sine=frequency=60:duration=1",
                                                                  "-c:v", "mpeg4", "-c:a", "aac", "-shortest"});
        const QString p2 = dir + "/p2.schneidi";
        run("new", {{"project", p2}, {"fps", 25}});
        edit(p2, {QJsonObject{{"op", "place"}, {"media", video}, {"at", 0}}});
        const auto videoClips = [&] {
            const QJsonObject info = run("info", {{"project", p2}});
            QStringList out;
            for (const QJsonValue& t : info["timeline"]["tracks"].toArray())
                if (t["track"].toString().startsWith('V'))
                    for (const QJsonValue& c : t["clips"].toArray()) out << QJsonDocument(c.toObject()).toJson(QJsonDocument::Compact);
            return out;
        };
        const QStringList before2 = videoClips();
        r = edit(p2, {QJsonObject{{"op", "sfx"}, {"at", "5s"}, {"kind", clip}}});
        CHECK_EQ(sfxNote(r)["placed"].toArray().size(), 1);
        CHECK(videoClips() == before2);
        CHECK_EQ(edit(p2, {QJsonObject{{"op", "sfx"}, {"at", "9s"}, {"kind", video}}}, true)["failed"].toString(), QString("NO_AUDIO"));
        // The end: a whoosh whose tail would run past the picture is left out
        r = edit(p2, {QJsonObject{{"op", "sfx"}, {"at", "13.96s"}, {"kind", "whoosh"}}});
        CHECK(sfxNote(r)["skipped"].toArray().size() == 1 && sfxNote(r)["skipped"][0]["why"].toString() == "after_end");
    }
    // A built-in sound whose cache file is gone (cache cleared, another computer): made again when the project loads
    {
        const QString file = SfxSynth::cachedFile(SfxSynth::Kind::Whoosh, 1);
        CHECK(QFile::remove(file));
        ProjectData d;
        CHECK(ProjectFile::load(project, &d, nullptr));
        CHECK(QFileInfo::exists(file));
        CHECK(ProjectFile::missingMedia(d).isEmpty());
        // Another version's name (another schneidi): left offline, never a different sound under the same clip
        CHECK(SfxSynth::restoreCachedFile("/x/sfx/whoosh-1-795ms-v1.wav").isEmpty());
        CHECK(SfxSynth::restoreCachedFile("/x/ding.wav").isEmpty());
        // A file of the version before (6) of a kind that has not changed since: restored as this version's file
        // (same samples); a riser of 6 (changed in 7) is not
        const QString click = SfxSynth::cachedFile(SfxSynth::Kind::Click, 2, 0.12);
        const QString oldName = QFileInfo(click).fileName().replace(QString("-v%1.wav").arg(SfxSynth::kVersion), "-v6.wav");
        CHECK(oldName != QFileInfo(click).fileName());
        CHECK_EQ(SfxSynth::restoreCachedFile("/elsewhere/sfx/" + oldName), click);
        const QString riser = QFileInfo(SfxSynth::cachedFile(SfxSynth::Kind::Riser, 1, 1.5)).fileName();
        CHECK(SfxSynth::restoreCachedFile("/elsewhere/sfx/" + QString(riser).replace(QString("-v%1.wav").arg(SfxSynth::kVersion), "-v6.wav")).isEmpty());
    }
    // `effects` lists the sounds
    const QJsonObject fx = run("effects", {{"kind", "sfx"}});
    CHECK_EQ(fx["sounds"].toArray().size(), 6);
    CHECK(run("effects", {})["sounds"].toArray().size() >= 6);
}

// The level follows the mix where a sound lands: as heard (momentary loudness) as loud as it (mild) or 3 LU louder
// (normal), master +3 dB; over silence the fixed level; volume_db wins
void testUnderMix(const QString& dir)
{
    // Pink noise: steady sound with peaks (a sine would hold the click under the peak cap at once)
    const QString media = Check::makeMedia(dir + "/loud.mkv", {"-f", "lavfi", "-i", "color=c=black:s=160x90:r=25:d=8",
                                                               "-f", "lavfi", "-i", "anoisesrc=d=8:c=pink:a=0.3:seed=7",
                                                               "-c:v", "ffv1", "-c:a", "pcm_s16le", "-shortest"});
    if (!CHECK(!media.isEmpty())) return;
    const QString project = dir + "/under.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    CHECK(!edit(project, {QJsonObject{{"op", "place"}, {"media", media}, {"at", 0}},
                          QJsonObject{{"op", "mixer"}, {"track", "master"}, {"volume_db", 3}}}).contains("failed"));
    const auto loud = [&](const QString& tracks, const char* from, const char* to) {
        QJsonObject args{{"source", project}, {"from", from}, {"to", to}};
        if (!tracks.isEmpty()) args["tracks"] = tracks;
        return run("loudness", args).value("loudness").toObject();
    };
    // The sound as heard against the mix around it: as loud (mild) or 3 LU louder (normal), unless its peak would
    // stand more than 3 dB over the mix's; both measured as exported (the master's +3 dB in both)
    const auto check = [&](const QJsonObject& n, const char* from, const char* to, double under, const char* what) {
        if (!CHECK_EQ(n["placed"].toArray().size(), 1)) return;
        const QJsonObject mix = loud("A1", from, to), sound = loud("SFX", from, to);
        const double mixM = mix["max_momentary_lufs"].toDouble(), soundM = sound["max_momentary_lufs"].toDouble();
        const double mixPeak = mix["sample_peak_db"].toDouble(), soundPeak = sound["sample_peak_db"].toDouble();
        const bool asLoud = std::abs(soundM - (mixM - under)) < 0.6, capped = std::abs(soundPeak - (mixPeak + 3)) < 0.6;
        if (!CHECK(mixM > -40 && soundM <= mixM - under + 0.6 && (asLoud || capped)))
            std::printf("       %s: sound %.2f LUFS peak %.2f, mix %.2f LUFS peak %.2f\n", what, soundM, soundPeak, mixM, mixPeak);
    };
    QJsonObject n = sfxNote(edit(project, {QJsonObject{{"op", "sfx"}, {"at", "2s"}, {"kind", "whoosh"}}}));
    check(n, "1.2s", "3s", 0, "mild");
    CHECK(n["level"].toString().contains("as loud as the mix"));
    n = sfxNote(edit(project, {QJsonObject{{"op", "sfx"}, {"at", "5s"}, {"kind", "whoosh"}, {"level", "normal"}}}));
    check(n, "4.2s", "6s", -3, "normal");
    // volume_db given: that level, the mix does not matter
    n = sfxNote(edit(project, {QJsonObject{{"op", "sfx"}, {"at", "7.4s"}, {"kind", "click"}, {"volume_db", -20}}}));
    if (CHECK_EQ(n["placed"].toArray().size(), 1))
        CHECK(std::abs(n["placed"][0]["clip_db"].toDouble() - (-20 + SfxSynth::levelGainDb(SfxSynth::Kind::Click, 1, 0))) < 0.01);
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
    // Installed again over itself: the new one takes the place, nothing of the old one is left beside it
    CHECK(Extensions::installSoundPack(pack("cc0", true), target).isEmpty());
    CHECK(QDir(Extensions::soundPacksDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot) == QStringList{"test-pack"});
    // Removed: gone, nothing left beside it
    Extensions::Item item;
    item.id = "test-pack";
    item.kind = Extensions::Kind::SoundPack;
    CHECK(Extensions::remove(item));
    CHECK(!QFileInfo::exists(target));
    CHECK(QDir(Extensions::soundPacksDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    // What a removal or reinstall could not delete (Windows: a file still open) goes at the next start
    CHECK(QDir().mkpath(target + ".old-1234abcd/cc0"));
    Extensions::cleanUp();
    QElapsedTimer t;
    t.start();
    while (QFileInfo::exists(target + ".old-1234abcd") && t.elapsed() < 5000) QThread::msleep(10);
    CHECK(!QFileInfo::exists(target + ".old-1234abcd"));
    // A licence removed by hand: the pack no longer counts
    CHECK(Extensions::installSoundPack(pack("cc0", true), target).isEmpty());
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
        << "Zuerst die mitgelieferten CC0-Aufnahmen (Whoosh, Swish, Hit: Standard), dann die berechneten (Ports von "
           "earshot-Rezepten; `core/SfxWhoosh`, `core/SfxForge`; für Riser, Impact, Klick und als Ersatz), 48 kHz mono. "
        << "`alle.wav` = alle Kandidaten nacheinander, je 1 s Stille dazwischen (Reihenfolge wie unten).\n\n"
        << "| # | Datei | Art | Startwert | Länge s | Spitze dBFS | RMS dBFS | Anker s | Quelle / Lizenz |\n"
        << "|---|---|---|---|---|---|---|---|---|\n";
    int n = 0;
    // The recorded ones first (whoosh, swish, hit: the default), decoded with ffmpeg for alle.wav
    for (SfxSynth::Kind k : {SfxSynth::Kind::Whoosh, SfxSynth::Kind::Swish, SfxSynth::Kind::Hit}) {
        unsigned seed = 0;
        for (const SfxSynth::Recorded& r : SfxSynth::recorded(k)) {
            ++seed;
            const QString file = QString("%1-%2-%3-%4").arg(++n, 2, 10, QChar('0')).arg(SfxSynth::kindName(k)).arg(seed)
                                     .arg(QFileInfo(r.file).fileName());
            QFile::remove(dir + "/" + file);
            QFile::copy(SfxSynth::recordedRoot() + "/" + r.file, dir + "/" + file);
            QProcess p;
            p.start("ffmpeg", {"-nostdin", "-loglevel", "error", "-i", dir + "/" + file, "-ac", "1", "-ar", "48000", "-f", "f32le", "-"});
            p.waitForFinished(30000);
            const QByteArray raw = p.readAllStandardOutput();
            std::vector<float> pcm(size_t(raw.size()) / sizeof(float));
            std::memcpy(pcm.data(), raw.constData(), pcm.size() * sizeof(float));
            const float g = float(std::pow(10.0, (SfxSynth::peakLevelDb(k) - r.peakDb) / 20)); // as op sfx levels it
            for (float& v : pcm) v *= g;
            all.insert(all.end(), pcm.begin(), pcm.end());
            all.insert(all.end(), gap.begin(), gap.end());
            out << QString("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | aufgenommen: %9, %10 (Datei unverändert; in alle.wav auf die Art-Spitze gepegelt) |\n")
                       .arg(n)
                       .arg(file, SfxSynth::kindName(k))
                       .arg(seed)
                       .arg(r.length, 0, 'f', 2)
                       .arg(r.peakDb, 0, 'f', 1)
                       .arg(pcm.empty() ? 0.0 : rmsDb(pcm) - 20 * std::log10(g), 0, 'f', 1)
                       .arg(r.anchor, 0, 'f', 3)
                       .arg(r.author, r.licence);
        }
    }
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
        static const char* recipes[] = {"whoosh (pass engine)", "riser (build) / darkriser", "knock", "subdrop", "shutter", "whoosh (pass engine)"};
        out << QString("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 |\n")
                   .arg(n)
                   .arg(file, SfxSynth::kindName(c.kind))
                   .arg(c.seed)
                   .arg(s.length, 0, 'f', 2)
                   .arg(quantizedPeakDb(s.samples), 0, 'f', 1)
                   .arg(rmsDb(s.samples), 0, 'f', 1)
                   .arg(s.anchor, 0, 'f', 3)
                   .arg(QString("berechnet, Port von earshots Rezept %1 (Leon Lin, MIT) + Parametersatz der Bank; "
                                "Code AGPL/MIT")
                            .arg(recipes[int(c.kind)]));
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
    testForgeRisers();
    testRiserDefault();
    testRecorded();
    testPlace();
    QTemporaryDir tmp;
    testPackLicense(tmp.path());
    if (Check::haveFfmpeg()) {
        testOp(tmp.path());
        testKillsDryRun(tmp.path());
        testUnderMix(tmp.path());
    } else {
        std::printf("  no ffmpeg: op test skipped\n");
    }
    if (const QString dir = qEnvironmentVariable("SCHNEIDI_SFX_SAMPLE_DIR"); !dir.isEmpty()) writeSample(dir);
    return Check::result();
}
