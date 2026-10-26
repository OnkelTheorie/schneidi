// `beats` (cli/AnalysisBeats.cpp, core/Beats.cpp): a synthetic song of known tempo and phase - a kick on every beat
// (the bar one louder), bass and hi-hats on the off-beats, a lead from bar 8, a break of 8 bars (only hi-hats and a
// quiet pad), then a louder drop with a crash. Checks BPM, phase, bar ones, phrases, energy per bar, the break and
// the drop, frames in a project's rate, a range, all beats, octaves (90 BPM with a sharp off-beat bass stays 90,
// 200 BPM with a kick on every beat stays 200), the features independent of the block size, a project's timeline
// sound and markers on it (one edit, undone by restore), errors.
#include "cli/Commands.h"
#include "core/Beats.h"

#include "check.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtEndian>

#include <cmath>
#include <random>
#include <vector>

namespace {

constexpr int kRate = 44100; // like most songs (the analysis decodes at 48 kHz)
constexpr double kPi = 3.14159265358979323846;
constexpr int kBars = 40, kBreakFrom = 16, kBreakTo = 24, kLeadFrom = 8;

// The song: `phase` = first beat, `accent` = the bar one's kick louder than the others
std::vector<float> song(double bpm, double phase, bool accent = true)
{
    const double beat = 60.0 / bpm;
    std::vector<float> s(size_t((phase + kBars * 4 * beat + 1.0) * kRate), 0.f);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-1.f, 1.f);
    const auto add = [&](double t0, double len, const auto& f) {
        const size_t a = size_t(std::lround(t0 * kRate)), n = size_t(len * kRate);
        for (size_t i = 0; i < n && a + i < s.size(); ++i) s[a + i] += float(f(double(i) / kRate));
    };
    for (int bar = 0; bar < kBars; ++bar) {
        const bool pause = bar >= kBreakFrom && bar < kBreakTo, drop = bar >= kBreakTo;
        const double gain = drop ? 1.0 : 0.5;
        for (int b = 0; b < 4; ++b) {
            const double t = phase + (bar * 4 + b) * beat;
            if (!pause) {
                // Kick: a sine falling from 150 to 50 Hz
                const double a = gain * (accent && b == 0 ? 1.0 : 0.7);
                add(t, 0.25, [&](double x) {
                    return a * std::exp(-x / 0.08) * std::sin(2 * kPi * (50 * x + 3 * (1 - std::exp(-x / 0.03))));
                });
                add(t + beat / 2, beat / 2, [&](double x) {
                    return 0.3 * gain * std::sin(2 * kPi * 55 * x) * std::min(1.0, x / 0.01);
                });
                if (bar >= kLeadFrom)
                    add(t, beat * 0.9, [&](double x) { return 0.05 * gain * (std::sin(2 * kPi * 880 * x) > 0 ? 1 : -1); });
            } else {
                add(t, beat, [&](double x) { return 0.04 * std::sin(2 * kPi * 440 * x); });
            }
            add(t + beat / 2, 0.03, [&](double x) { return 0.08 * noise(rng) * std::exp(-x / 0.01); });
        }
        if (bar == kBreakTo) add(phase + bar * 4 * beat, 1.5, [&](double x) { return 0.3 * noise(rng) * std::exp(-x / 0.4); });
    }
    return s;
}

bool writeWav(const QString& path, const std::vector<float>& mono)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 data = quint32(mono.size() * 2);
    const auto le32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        f.write(b, 4);
    };
    const auto le16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        f.write(b, 2);
    };
    f.write("RIFF");
    le32(36 + data);
    f.write("WAVEfmt ");
    le32(16);
    le16(1); // PCM
    le16(1); // mono
    le32(kRate);
    le32(kRate * 2);
    le16(2);
    le16(16);
    f.write("data");
    le32(data);
    for (float v : mono) le16(quint16(qint16(std::lround(std::clamp(v, -1.f, 1.f) * 32767))));
    return true;
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) {
        std::printf("no command %s\n", qPrintable(name));
        return {{"failed", "NO_COMMAND"}};
    }
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"failed", e.code}};
    }
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Features: the same values however the signal is cut into blocks
void testBlocks()
{
    const std::vector<float> s = song(128, 0.25);
    std::vector<float> part(s.begin(), s.begin() + 10 * kRate);
    Beats::Features one(kRate), many(kRate);
    one.add(part.data(), int(part.size()));
    for (size_t i = 0; i < part.size(); i += 777) many.add(part.data() + i, int(std::min<size_t>(777, part.size() - i)));
    const Beats::Values a = one.take(), b = many.take();
    CHECK_EQ(int(a.onset.size()), 10 * Beats::kRate);
    CHECK(a.onset == b.onset);
    CHECK(a.energy == b.energy);
    CHECK(a.bass == b.bass);
    CHECK(a.high == b.high);
    CHECK(!Beats::analyze(Beats::Values{}).found);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("beats");
    Check::initMlt();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8()); // backups of the project edits
    testBlocks();

    const double bpm = 128, phase = 0.25, beat = 60.0 / bpm, bar = 4 * beat;
    const QString file = dir.filePath("song.wav");
    if (!CHECK(writeWav(file, song(bpm, phase)))) return Check::result();
    const double breakFrom = phase + kBreakFrom * bar, dropAt = phase + kBreakTo * bar;

    // A media file: frames in its own rate (no picture: 25 fps)
    QJsonObject r = run("beats", {{"file", file}});
    CHECK(near(r["bpm"].toDouble(), bpm, 0.05));
    CHECK(r["confidence"].toDouble() > 0.5);
    CHECK(near(r["beat_phase"]["s"].toDouble(), phase, 0.01));
    CHECK_EQ(r["beat_phase"]["frame"].toInt(), int(std::lround(phase * 25)));
    CHECK(near(r["beat_period"]["s"].toDouble(), beat, 0.0005));
    CHECK(near(r["beat_period"]["frames"].toDouble(), beat * 25, 0.01));
    CHECK_EQ(r["fps"].toDouble(), 25.0);
    // Bar ones: the louder kick, where the sound changes
    const QJsonObject down = r["downbeats"].toObject();
    CHECK(near(down["s"].toDouble(), phase, 0.01));
    CHECK(near(down["every_s"].toDouble(), bar, 0.002));
    CHECK(down["confidence"].toDouble() > 0.3);
    // Phrases of 8 bars from bar 1 (the lead at bar 9, the break at 17, the drop at 25)
    CHECK_EQ(r["phrase_bars"].toInt(), 8);
    const QJsonArray phrases = r["phrases"].toArray();
    if (CHECK_EQ(phrases.size(), 5)) {
        for (int i = 0; i < 5; ++i) {
            CHECK_EQ(phrases[i]["bar"].toInt(), 1 + 8 * i);
            CHECK(near(phrases[i]["s"].toDouble(), phase + 8 * i * bar, 0.01));
        }
        CHECK(phrases[2]["energy"].toInt() < phrases[1]["energy"].toInt()); // the break
        CHECK(phrases[3]["energy"].toInt() > phrases[1]["energy"].toInt()); // the drop: louder than before
        CHECK_EQ(phrases[3]["bass"].toInt(), 9);
        CHECK_EQ(phrases[2]["bass"].toInt(), 0);
    }
    // Energy per bar: one digit per bar, a space before every phrase
    const QString energy = r["energy"]["overall"].toString(), bass = r["energy"]["bass"].toString();
    CHECK_EQ(QString(energy).remove(' ').size(), kBars);
    CHECK_EQ(energy.count(' '), 4);
    CHECK_EQ(QString(bass).remove(' ').mid(kBreakFrom, kBreakTo - kBreakFrom), QString(8, '0'));
    CHECK_EQ(QString(bass).remove(' ').mid(kBreakTo, 8), QString(8, '9'));
    // The break and the drop after it
    const QJsonArray breaks = r["breaks"].toArray();
    if (CHECK_EQ(breaks.size(), 1)) {
        CHECK(near(breaks[0]["from_s"].toDouble(), breakFrom, 0.02));
        CHECK(near(breaks[0]["to_s"].toDouble(), dropAt, 0.02));
        CHECK_EQ(breaks[0]["bars"].toDouble(), 8.0);
        CHECK(breaks[0]["bass_db"].toDouble() < -20);
    }
    const QJsonArray drops = r["drops"].toArray();
    if (CHECK_EQ(drops.size(), 1)) {
        CHECK(near(drops[0]["s"].toDouble(), dropAt, 0.02));
        CHECK_EQ(drops[0]["frame"].toInt(), int(std::lround(dropAt * 25)));
        CHECK_EQ(drops[0]["bar"].toInt(), kBreakTo + 1);
        CHECK_EQ(drops[0]["beat"].toInt(), 1);
        CHECK(drops[0]["bass_jump_db"].toDouble() > 20);
        CHECK(near(drops[0]["break_s"].toDouble(), 8 * bar, 0.05));
    }
    CHECK(!r.contains("beats")); // only when asked

    // Frames in a project's rate, all beats
    const QString project = dir.filePath("p.schneidi");
    run("new", {{"project", project}, {"fps", 30}});
    r = run("beats", {{"file", file}, {"project", project}, {"beats", true}});
    CHECK_EQ(r["fps"].toDouble(), 30.0);
    if (CHECK_EQ(r["drops"].toArray().size(), 1)) CHECK_EQ(r["drops"][0]["frame"].toInt(), int(std::lround(dropAt * 30)));
    const QJsonArray beats = r["beats"].toArray();
    CHECK_EQ(beats.size(), kBars * 4);
    if (beats.size() > 8) {
        CHECK(near(beats[0]["s"].toDouble(), phase, 0.01));
        CHECK(near(beats[8]["s"].toDouble(), phase + 8 * beat, 0.01));
        CHECK_EQ(beats[8]["frame"].toInt(), int(std::lround(beats[8]["s"].toDouble() * 30)));
        CHECK(beats[4]["bar_one"].toBool());
        CHECK(!beats[5]["bar_one"].toBool());
    }
    // A range of the file: times stay the file's
    r = run("beats", {{"file", file}, {"from", "20s"}, {"to", "60s"}});
    CHECK(near(r["bpm"].toDouble(), bpm, 0.1));
    CHECK(near(std::fmod(r["beat_phase"]["s"].toDouble() - phase + 100 * beat, beat), 0, 0.01)
          || near(std::fmod(r["beat_phase"]["s"].toDouble() - phase + 100 * beat, beat), beat, 0.01));
    CHECK(r["beat_phase"]["s"].toDouble() >= 20);
    if (CHECK(!r["drops"].toArray().isEmpty())) CHECK(near(r["drops"][0]["s"].toDouble(), dropAt, 0.02));

    // Octaves: a sharp off-beat bass does not double 90 BPM; a kick on every beat keeps 200 BPM (not 100)
    const QString slow = dir.filePath("slow.wav"), fast = dir.filePath("fast.wav");
    if (CHECK(writeWav(slow, song(90, 0.4)) && writeWav(fast, song(200, 0.033, false)))) {
        r = run("beats", {{"file", slow}});
        CHECK(near(r["bpm"].toDouble(), 90, 0.05));
        CHECK(near(r["beat_phase"]["s"].toDouble(), 0.4, 0.01));
        r = run("beats", {{"file", fast}});
        CHECK(near(r["bpm"].toDouble(), 200, 0.05));
        CHECK(near(r["beat_phase"]["s"].toDouble(), 0.033, 0.01));
        r = run("beats", {{"file", fast}, {"max_bpm", 150}});
        CHECK(near(r["bpm"].toDouble(), 100, 0.05)); // the range asked for
    }

    // A project: its timeline sound, times in timeline frames (the song from 2 s on), markers on the phrase starts
    run("edit", {{"project", project}, {"ops", QString(R"([{"op":"place","media":"%1","at":60}])").arg(file)}});
    r = run("beats", {{"file", project}, {"from", 60}});
    CHECK(near(r["bpm"].toDouble(), bpm, 0.05));
    CHECK(near(r["beat_phase"]["s"].toDouble(), 2 + phase, 0.01));
    if (CHECK(!r["drops"].toArray().isEmpty())) CHECK_EQ(r["drops"][0]["frame"].toInt(), int(std::lround((2 + dropAt) * 30)));
    CHECK(!r.contains("applied"));
    r = run("beats", {{"file", project}, {"from", 60}, {"apply", "markers"}});
    CHECK_EQ(r["applied"]["markers"].toInt(), 5);
    CHECK(r["applied"]["saved"].toBool());
    QJsonArray markers = run("info", {{"project", project}})["timeline"]["markers"].toArray();
    if (CHECK_EQ(markers.size(), 5))
        for (int i = 0; i < 5; ++i) CHECK_EQ(markers[i].toInt(), int(std::lround((2 + phase + 8 * i * bar) * 30)));
    run("restore", {{"project", project}}); // one edit = one step back
    CHECK(run("info", {{"project", project}})["timeline"]["markers"].toArray().isEmpty());
    r = run("beats", {{"file", project}, {"from", 60}, {"apply", "markers"}, {"markers", "bars"}});
    CHECK_EQ(r["applied"]["markers"].toInt(), kBars);
    markers = run("info", {{"project", project}})["timeline"]["markers"].toArray();
    if (CHECK_EQ(markers.size(), kBars)) CHECK_EQ(markers[1].toInt(), int(std::lround((2 + phase + bar) * 30)));

    // Errors
    CHECK_EQ(run("beats", {{"file", file}, {"apply", "markers"}})["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(run("beats", {{"file", project}, {"apply", "cuts"}})["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(run("beats", {{"file", project}, {"apply", "markers"}, {"markers", "beats"}})["failed"].toString(),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(run("beats", {{"file", file}, {"from", "30s"}, {"to", "10s"}})["failed"].toString(), QString("BAD_ARGUMENT"));
    const QString quiet = dir.filePath("quiet.wav");
    if (CHECK(writeWav(quiet, std::vector<float>(size_t(10 * kRate), 0.f))))
        CHECK_EQ(run("beats", {{"file", quiet}})["failed"].toString(), QString("NO_BEAT"));
    return Check::result();
}
