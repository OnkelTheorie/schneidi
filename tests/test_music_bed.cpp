// SPDX-License-Identifier: AGPL-3.0-only
// Music beds (core/MusicBed, edit op `music_bed`): per mood a bed of 30 s - the exact sample count, the end at silence,
// true peak at most -1 dBTP (BS.1770 meter), loudness about -16 LUFS, no DC, no clicks (sample jumps), deterministic
// (the same options give the same samples, another seed others), the known grid on the onsets the beat analysis hears
// (core/Beats on the bed), the final hit on a bar one. The op: lengths at 25/30/60 fps exact to a frame (clip and file),
// length:'timeline' over a speech clip with ducking (volume keyframes) on a new track "Musik", the grid in the beats
// cache (no analysis), a second call reuses the file, duck:false, gain, errors, `effects kind:'music'`. Speed: a
// 3-minute bed (printed; checked loosely, the number that counts is measured idle).
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/AppPaths.h"
#include "core/Beats.h"
#include "core/Keyframes.h"
#include "core/Loudness.h"
#include "core/MusicBed.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TruePeak.h"

#include "check.h"
#include "synth_sounds.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

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

QJsonObject edit(const QString& project, const QString& ops, bool quiet = false)
{
    return run("edit", {{"project", project}, {"ops", ops}}, quiet);
}

QJsonObject bedNote(const QJsonObject& r)
{
    for (const QJsonValue& v : r["notes"].toArray())
        if (v.toObject().contains("music_bed")) return v.toObject()["music_bed"].toObject();
    return {};
}

std::unique_ptr<Project> load(const QString& path)
{
    ProjectData d;
    ProjectFile::load(path, &d, nullptr);
    auto p = std::make_unique<Project>();
    p->load(d);
    return p;
}

const Clip* clipById(const Project& p, int id, int* track = nullptr)
{
    for (int t = 0; t < p.timeline().audio.size(); ++t)
        for (const Clip& c : p.timeline().audio[t].clips)
            if (c.id == id) {
                if (track) *track = t;
                return &c;
            }
    return nullptr;
}

double truePeak(const std::vector<float>& s)
{
    TruePeakMeter m;
    m.addInterleaved(s.data(), int(s.size() / 2), 2, MusicBed::kRate);
    return m.maxDb();
}

double lufs(const std::vector<float>& s)
{
    LoudnessMeter m;
    m.addInterleaved(s.data(), int(s.size() / 2), 2, MusicBed::kRate);
    return m.integrated();
}

// The core checks of one bed
void checkBed(MusicBed::Mood mood)
{
    const QString name = MusicBed::moodId(mood);
    const MusicBed::Result r = MusicBed::make({mood, 30});
    const long long frames = (long long)r.samples.size() / 2;
    CHECK_EQ(frames, 30LL * MusicBed::kRate);
    CHECK_EQ(r.samples.size() % 2, size_t(0));
    // Levels
    const double tp = truePeak(r.samples), l = lufs(r.samples);
    if (!CHECK(tp <= MusicBed::kCeilingDbtp + 0.05)) std::printf("       %s: true peak %.2f dBTP\n", qPrintable(name), tp);
    if (!CHECK(l > -17.5 && l < -15.0)) std::printf("       %s: %.2f LUFS\n", qPrintable(name), l);
    CHECK(std::abs(r.lufs - l) < 0.2);
    CHECK(std::abs(r.truePeakDb - tp) < 0.2);
    // No DC, ends at silence, starts at silence
    double sumL = 0, sumR = 0;
    for (long long f = 0; f < frames; ++f) sumL += r.samples[size_t(2 * f)], sumR += r.samples[size_t(2 * f + 1)];
    if (!CHECK(std::abs(sumL / frames) < 1e-3 && std::abs(sumR / frames) < 1e-3))
        std::printf("       %s: DC %.5f / %.5f\n", qPrintable(name), sumL / frames, sumR / frames);
    CHECK_EQ(r.samples[r.samples.size() - 1], 0.f);
    CHECK_EQ(r.samples[r.samples.size() - 2], 0.f);
    CHECK(std::abs(r.samples[0]) < 1e-3 && std::abs(r.samples[1]) < 1e-3);
    // No clicks: no jump from one sample to the next stands out of the sound around it (a step at a note boundary
    // would be far above the 5 ms around it)
    {
        double worst = 0;
        long long at = 0;
        const int w = 240;
        for (int c = 0; c < 2; ++c) {
            std::vector<double> d(size_t(frames), 0.0);
            for (long long f = 2; f < frames; ++f) // second difference: steps and kinks, not smooth waves
                d[size_t(f)] = std::abs(double(r.samples[size_t(2 * f + c)]) - 2 * r.samples[size_t(2 * (f - 1) + c)]
                                        + r.samples[size_t(2 * (f - 2) + c)]);
            double run = 0;
            for (long long f = 0; f < std::min<long long>(frames, 2 * w + 1); ++f) run += d[size_t(f)];
            for (long long f = w; f + w < frames; ++f) {
                const double mean = run / (2 * w + 1);
                const double ratio = d[size_t(f)] / (mean + 1e-4);
                if (ratio > worst) worst = ratio, at = f;
                run += d[size_t(f + w + 1 < frames ? f + w + 1 : f)] - d[size_t(f - w)];
            }
        }
        if (!CHECK(worst < 30)) std::printf("       %s: a click at %.4f s (%.1f x the local level)\n", qPrintable(name), double(at) / MusicBed::kRate, worst);
    }
    // Deterministic, and another seed sounds different
    const MusicBed::Result again = MusicBed::make({mood, 30});
    CHECK(again.samples.size() == r.samples.size()
          && std::memcmp(again.samples.data(), r.samples.data(), r.samples.size() * sizeof(float)) == 0);
    MusicBed::Options other{mood, 30};
    other.seed = 2;
    const MusicBed::Result two = MusicBed::make(other);
    CHECK(two.samples.size() == r.samples.size()
          && std::memcmp(two.samples.data(), r.samples.data(), r.samples.size() * sizeof(float)) != 0);
    // The grid: bar ones at k % 4 == 0 from 0, and the beat analysis hears the same tempo (or an octave of it) and
    // beats on it
    const Beats::Result& g = r.beats;
    CHECK(g.found && g.phase == 0 && g.downbeat == 0);
    CHECK(std::abs(g.period - 60.0 / MusicBed::defaultBpm(mood)) < 1e-9);
    CHECK_EQ(g.beats, int(std::floor((30 - 1e-6) / g.period)));
    CHECK_EQ(int(g.barEnergyDb.size()), int(std::floor(30 / (4 * g.period) + 1e-9)));
    {
        Beats::Features features(MusicBed::kRate);
        std::vector<float> mono(static_cast<size_t>(frames));
        for (long long f = 0; f < frames; ++f) mono[size_t(f)] = 0.5f * (r.samples[size_t(2 * f)] + r.samples[size_t(2 * f + 1)]);
        features.add(mono.data(), int(mono.size()));
        const Beats::Result heard = Beats::analyze(features.take());
        const double ratio = heard.bpm / g.bpm;
        const bool octave = std::abs(ratio - 1) < 0.01 || std::abs(ratio - 2) < 0.02 || std::abs(ratio - 0.5) < 0.005;
        // Where the heard beats lie on our grid: within 20 ms of a grid beat (or of a half beat when it hears double)
        const double unit = std::abs(ratio - 2) < 0.02 ? g.period / 2 : g.period;
        double off = std::fmod(heard.phase, unit);
        off = std::min(off, unit - off);
        if (!CHECK(heard.found && octave && off < 0.02))
            std::printf("       %s: heard %.2f BPM (grid %.2f, confidence %.2f), phase %.3f s off the grid by %.3f s\n",
                        qPrintable(name), heard.bpm, g.bpm, heard.confidence, heard.phase, off);
    }
    // The final hit on a bar one, the ending section up to the end
    CHECK(r.endBar > 0 && !r.sections.empty() && r.sections.back().kind == MusicBed::Section::Ending
          && r.sections.back().fromBar == r.endBar);
    // describe() gives the same plan without the sound
    const MusicBed::Result d = MusicBed::describe({mood, 30});
    CHECK(d.samples.empty() && d.chords == r.chords && d.endBar == r.endBar && d.beats.period == g.period);
}

} // namespace

int main(int argc, char* argv[])
{
    Check::isolateSettings();
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    AppPaths::setProgramNames();

    // ---------- Core ----------
    for (const MusicBed::Mood m : MusicBed::kMoods) checkBed(m);
    // Keys, moods, names
    {
        const auto k = [](const char* s) { return MusicBed::keyFrom(QString::fromUtf8(s)); };
        CHECK(k("A minor") == (MusicBed::Key{9, true}));
        CHECK(k("F#m") == (MusicBed::Key{6, true}));
        CHECK(k("Bb") == (MusicBed::Key{10, false}));
        CHECK(k("Eb major") == (MusicBed::Key{3, false}));
        CHECK(k("c-moll") == (MusicBed::Key{0, true}));
        CHECK(k("D-Dur") == (MusicBed::Key{2, false}));
        CHECK(!k("H minor") && !k("") && !k("A lydian"));
        CHECK_EQ(MusicBed::keyName({10, true}), QString("Bb minor"));
        CHECK(MusicBed::moodFrom("treibend") == MusicBed::Mood::Drive && MusicBed::moodFrom("Ruhig") == MusicBed::Mood::Calm);
        CHECK(!MusicBed::moodFrom("sad"));
        // Short beds: no final hit, faded; still the exact length and silence at the end
        const MusicBed::Result s = MusicBed::make({MusicBed::Mood::Hype, 3.3});
        CHECK_EQ((long long)s.samples.size(), 2 * MusicBed::frameCount(3.3));
        CHECK(s.endBar < 0 && s.samples.back() == 0.f);
        CHECK(truePeak(s.samples) <= MusicBed::kCeilingDbtp + 0.05);
        // A given tempo and key
        MusicBed::Options o{MusicBed::Mood::Drive, 20};
        o.bpm = 95;
        o.key = MusicBed::Key{2, false};
        const MusicBed::Result g = MusicBed::describe(o);
        CHECK(g.bpm == 95 && g.key == (MusicBed::Key{2, false}) && g.chords.value(0) == "D");
        CHECK(MusicBed::fileName(o).startsWith("music-bed-drive-95bpm-d-major-s1-20000ms-v"));
        // A long bed has breaks (with a drop after each)
        const MusicBed::Result longBed = MusicBed::make({MusicBed::Mood::Hype, 90});
        int breaks = 0;
        for (const MusicBed::Section& sec : longBed.sections) breaks += sec.kind == MusicBed::Section::Break;
        CHECK(breaks >= 1 && !longBed.beats.drops.empty());
    }
    // Speed: 3 minutes (well under a second on an idle PC; loose here, tests run in parallel)
    {
        QElapsedTimer t;
        t.start();
        const MusicBed::Result r = MusicBed::make({MusicBed::Mood::Hype, 180});
        const double ms = t.nsecsElapsed() / 1e6;
        std::printf("       3-minute hype bed: %.0f ms\n", ms);
        CHECK(r.samples.size() == size_t(2 * 180 * MusicBed::kRate) && ms < 5000);
    }

    // ---------- The op ----------
    for (const int fps : {25, 30, 60}) {
        const QString project = tmp.path() + QString("/fps%1.schneidi").arg(fps);
        run("new", {{"project", project}, {"fps", fps}});
        // 17.3 s: not a whole number of frames at every rate before rounding; the clip and the file are that long
        const QJsonObject r = edit(project, R"([{"op":"music_bed","mood":"drive","length":"17.3s"}])");
        const QJsonObject n = bedNote(r);
        const int want = int(std::lround(17.3 * fps));
        const auto p = load(project);
        const Clip* c = clipById(*p, n["clip"].toInt());
        if (!CHECK(c && c->length() == want && c->start == 0))
            std::printf("       %d fps: %s\n", fps, QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        // The file as the project reads it (frames at its rate)
        const MediaInfo* m = p->mediaInfo(n["file"].toString());
        if (!CHECK(m && std::abs(m->length - want) <= 1)) std::printf("       %d fps: file %d frames\n", fps, m ? m->length : -1);
        CHECK(n["made"].toBool());
        // Nothing else on the timeline: not ducked
        CHECK(c && !Keys::animated(*c, AnimParam::Volume));
    }

    // length:'timeline' over a speech clip: a new track "Musik", ducked under the speech
    const QString speech = tmp.path() + "/speech.wav";
    {
        Synth::Builder b(48000, 20.0, 3);
        Synth::speech(b, 0.5, 6.0);
        Synth::speech(b, 12.0, 6.0);
        CHECK(Synth::writeWav(speech, b.s, 48000));
    }
    const QString project = tmp.path() + "/talk.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    edit(project, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"}])").arg(speech));
    const auto before = load(project);
    const int audioTracks = before->timeline().audio.size();
    QJsonObject r = edit(project, R"([{"op":"music_bed","mood":"calm"}])");
    QJsonObject n = bedNote(r);
    {
        const auto p = load(project);
        int track = -1;
        const Clip* c = clipById(*p, n["clip"].toInt(), &track);
        if (!CHECK(c && c->length() == 20 * 25 && c->start == 0))
            std::printf("       %s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        CHECK_EQ(track, audioTracks); // a new track after the others
        CHECK_EQ(p->timeline().audio.size(), audioTracks + 1);
        CHECK_EQ(p->timeline().audio.value(track).name, T("Musik"));
        if (!CHECK(c && Keys::animated(*c, AnimParam::Volume))) std::printf("       ducking: %s\n", qPrintable(n["ducking"].toString()));
        CHECK(n["ducking"].toString().startsWith("ducked"));
        // The file next to the project, its grid in the cache: snap_to_beat and fit_music do not analyse
        const QString file = n["file"].toString();
        CHECK_EQ(QFileInfo(file).absolutePath(), QFileInfo(tmp.path() + "/talk-music").absoluteFilePath());
        const std::optional<Beats::Result> cached = Cli::detail::cachedSongBeats(file, 0);
        CHECK(cached && std::abs(cached->bpm - 84) < 1e-9 && cached->phase == 0 && !cached->barEnergyDb.empty());
        const QJsonObject beats = run("beats", {{"file", file}});
        CHECK(beats["bpm"].toDouble() == 84 && beats["confidence"].toDouble() == 1); // the stored grid, not analysed
        CHECK(n["sections"].toArray().size() >= 2 && n["final_hit_s"].toDouble() > 0);
        CHECK(n["lufs"].toDouble() < -15 && n["lufs"].toDouble() > -17.5);
    }
    // The same bed again (onto another track, not ducked): the file is reused, a volume given
    r = edit(project, R"([{"op":"music_bed","mood":"calm","length":"timeline","duck":false,"gain":-6,"track":"A9"}])");
    n = bedNote(r);
    {
        CHECK(!n["made"].toBool(true)); // reused
        const auto p = load(project);
        const Clip* c = clipById(*p, n["clip"].toInt());
        CHECK(c && c->volumeDb == -6 && !Keys::animated(*c, AnimParam::Volume));
    }
    // snap_to_beat takes the bed's grid: a cut 0.2 s off a beat moves onto it
    {
        const QString pj = tmp.path() + "/snap.schneidi";
        run("new", {{"project", pj}, {"fps", 25}});
        const QString clip = tmp.path() + "/clip.wav";
        Synth::Builder b(48000, 10.0, 5);
        Synth::speech(b, 0.2, 9.5);
        CHECK(Synth::writeWav(clip, b.s, 48000));
        // A titles as video clips: two solid titles, the cut at 2.3 s (120 BPM: beats every 0.5 s)
        edit(pj, R"([{"op":"title","text":"A","at":0,"duration":"2.3s","track":"V1"},{"op":"title","text":"B","at":"2.3s","duration":"3s","track":"V1"}])");
        const QJsonObject bed = edit(pj, R"([{"op":"music_bed","mood":"hype","bpm":120,"length":"8s"}])");
        const QJsonObject s = edit(pj, R"([{"op":"snap_to_beat","grid":"beat"}])");
        const auto p = load(pj);
        bool onBeat = false;
        for (const Clip& c : p->timeline().video.value(0).clips)
            if (c.start > 0) onBeat = c.start == 50 || c.start == 62 || c.start == 63; // 2.0 s or 2.5 s
        if (!CHECK(onBeat && bedNote(bed)["bpm"].toDouble() == 120))
            std::printf("       %s\n", QJsonDocument(s).toJson(QJsonDocument::Compact).constData());
    }
    // Errors
    CHECK_EQ(edit(project, R"([{"op":"music_bed"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"music_bed","mood":"sad","length":"5s"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"music_bed","mood":"calm","length":"5s","key":"X"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"music_bed","mood":"calm","length":"5s","bpm":300}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    {
        const QString empty = tmp.path() + "/empty.schneidi";
        run("new", {{"project", empty}});
        CHECK_EQ(edit(empty, R"([{"op":"music_bed","mood":"calm"}])", true)["failed"].toString(), QString("EMPTY"));
    }
    // effects kind:'music'
    {
        const QJsonObject e = run("effects", {{"kind", "music"}});
        const QJsonArray moods = e["moods"].toArray();
        CHECK_EQ(moods.size(), 3);
        CHECK_EQ(moods.at(0).toObject()["mood"].toString(), QString("calm"));
        CHECK_EQ(moods.at(2).toObject()["chords"].toArray().size(), 4);
    }
    return Check::result();
}
