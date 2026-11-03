// SPDX-License-Identifier: AGPL-3.0-only
// Edit ops `fit_music` and `snap_to_beat` (cli/EditOpsMusic.cpp, core/MusicFit): a synthetic song (120 BPM, 0.5 s of
// silence, seven phrases of 8 bars with their own level, pad tone and parts - intro, verse, build, break, drop, verse,
// outro - and an end that rings out 3.5 s) cut to 30 s, 52 s and with the drop on a climax. Checks: the length within
// a frame, every cut on a bar one (whole phrases when the length allows), the start kept, the last part = the song's own
// end, the parts on one beat grid (sub-frame positions), no click at the cuts (sample jumps of the mixed sound against
// the song's own), a too short song, end:'fade', one undo step through the edit, live in the app the same as on the
// file; snap_to_beat: cuts 0.28 s off the bar ones rolled onto them, a hit slipped onto the beat with its accent
// keyframe, a marker, max_shift.
#include "cli/Commands.h"
#include "cli/Live.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include "check.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtEndian>

#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;
constexpr double kLead = 0.5, kBeat = 0.5, kBar = 2.0, kRing = 3.5;
constexpr int kPhraseBars = 8, kPhrases = 7, kBars = kPhraseBars * kPhrases;
constexpr double kSongLength = kLead + kBars * kBar + kRing; // 116 s
constexpr int kFps = 50;                                     // beats on whole frames (25 per beat)

double barStart(int b) { return kLead + b * kBar; }

std::vector<float> song()
{
    std::vector<float> s(size_t(kSongLength * kRate), 0.f);
    const auto add = [&](double t0, double len, const auto& f) {
        const size_t a = size_t(std::lround(t0 * kRate)), n = size_t(len * kRate);
        for (size_t i = 0; i < n && a + i < s.size(); ++i) s[a + i] += float(f(double(i) / kRate, t0 + double(i) / kRate));
    };
    // Per phrase: level, pad tone, which parts play
    const double gain[kPhrases] = {0.45, 0.6, 0.7, 0.35, 1.0, 0.6, 0.45};
    const double pad[kPhrases] = {221.3, 262.7, 293.9, 247.4, 330.6, 262.7, 196.2}; // not in phase at the bar ones
    for (int bar = 0; bar < kBars; ++bar) {
        const int ph = bar / kPhraseBars;
        const bool pause = ph == 3, bass = ph != 0 && !pause, lead = ph == 2 || ph == 4;
        const double g = gain[ph];
        const double t0 = barStart(bar);
        // A pad that holds through the phrase (its phase runs on: a hard cut between two of them jumps)
        add(t0, kBar, [&](double, double t) {
            const double x = std::fmod(t - kLead, kPhraseBars * kBar), edge = std::min({1.0, x / 0.005, (kPhraseBars * kBar - x) / 0.005});
            return 0.22 * g * edge * std::sin(2 * kPi * pad[ph] * t);
        });
        for (int b = 0; b < 4; ++b) {
            const double t = t0 + b * kBeat;
            if (!pause) {
                const double a = g * (b == 0 ? 1.0 : 0.6);
                add(t, 0.25, [&](double x, double) {
                    return a * std::exp(-x / 0.08) * std::sin(2 * kPi * (50 * x + 3 * (1 - std::exp(-x / 0.03))));
                });
            }
            if (bass)
                add(t + kBeat / 2, kBeat / 2, [&](double x, double) {
                    return 0.3 * g * std::sin(2 * kPi * 55 * x) * std::min(1.0, x / 0.01) * std::min(1.0, (kBeat / 2 - x) / 0.01);
                });
            if (lead)
                add(t, kBeat * 0.9, [&](double x, double) {
                    return 0.05 * g * std::sin(2 * kPi * 880 * x) * std::min(1.0, x / 0.005) * std::exp(-x / 0.3);
                });
            // Hi-hat: a short high tone burst on the off-beat
            add(t + kBeat / 2, 0.03, [&](double x, double) { return 0.04 * std::sin(2 * kPi * 7000 * x) * std::exp(-x / 0.008); });
        }
        if (bar == 4 * kPhraseBars) // the drop: a crash of high tones
            add(t0, 1.5, [&](double x, double) {
                return 0.08 * std::exp(-x / 0.4) * (std::sin(2 * kPi * 3100 * x) + std::sin(2 * kPi * 4300 * x));
            });
    }
    // The end: a last kick and the pad ringing out
    const double end = barStart(kBars);
    add(end, 0.3, [&](double x, double) { return 0.5 * std::exp(-x / 0.08) * std::sin(2 * kPi * (50 * x + 3 * (1 - std::exp(-x / 0.03)))); });
    add(end, kRing, [&](double x, double t) { return 0.22 * 0.45 * std::min(1.0, x / 0.005) * std::exp(-x / 0.8) * std::sin(2 * kPi * 196.2 * t); });
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
    le16(1);
    le16(1);
    le32(kRate);
    le32(kRate * 2);
    le16(2);
    le16(16);
    f.write("data");
    le32(data);
    for (float v : mono) le16(quint16(qint16(std::lround(std::clamp(v, -1.f, 1.f) * 32767))));
    return true;
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

QJsonObject edit(const QString& project, const QString& ops, bool quiet = false)
{
    return run("edit", {{"project", project}, {"ops", ops}}, quiet);
}

// The fit_music / snap_to_beat part of an edit's notes
QJsonValue note(const QJsonObject& r, const QString& key)
{
    for (const QJsonValue& v : r["notes"].toArray())
        if (v.toObject().contains(key)) return v.toObject()[key];
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

// The mixed sound of the timeline (mono, 48 kHz)
std::vector<float> mix(const Project& p)
{
    std::vector<float> out;
    const Timeline tl = p.renderTimeline();
    AudioAnalysis::timelineSound(p.timelineFormat(), tl, 0, TimelineOps::endFrame(tl),
                                 [&](const float* pcm, int samples, int channels, int) {
                                     for (int i = 0; i < samples; ++i) {
                                         float v = 0;
                                         if (pcm)
                                             for (int c = 0; c < channels; ++c) v += pcm[size_t(c) * size_t(samples) + size_t(i)];
                                         out.push_back(pcm ? v / float(std::max(1, channels)) : 0.f);
                                     }
                                 });
    return out;
}

// Largest jump from one sample to the next in [from, to) seconds
double maxJump(const std::vector<float>& s, double from, double to)
{
    double m = 0;
    const size_t a = size_t(std::max(1.0, from * kRate)), b = std::min(s.size(), size_t(to * kRate));
    for (size_t i = a; i < b; ++i) m = std::max(m, double(std::abs(s[i] - s[i - 1])));
    return m;
}

// The audio clips of track A<n> in time order
QVector<Clip> clipsOn(const Project& p, int track)
{
    QVector<Clip> out = p.timeline().audio.value(track).clips;
    std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return out;
}

bool onBar(double t, int* bar = nullptr)
{
    const double b = (t - kLead) / kBar;
    if (bar) *bar = int(std::lround(b));
    return std::abs(b - std::round(b)) * kBar < 0.012;
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
    p.start(SCHNEIDI_CLI, args);
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

// One fit: the length, bar ones, start, end, the grid, the sound at the cuts
void checkFit(const QString& project, const QString& file, double length, double baseline, bool phrases)
{
    std::printf("  fit to %.1f s\n", length);
    run("edit", {{"project", project}, {"ops", "[{\"op\":\"clear\"}]"}});
    const QJsonObject r = edit(project, QString(R"([{"op":"fit_music","media":"%1","length":"%2s","track":"A1"}])").arg(file).arg(length));
    const QJsonObject fit = note(r, "fit_music").toObject();
    if (!CHECK(!fit.isEmpty())) return;
    const auto pPtr = load(project);
    const Project& p = *pPtr;
    const QVector<Clip> parts = clipsOn(p, 0);
    if (!CHECK(parts.size() >= 2)) return;
    // The length within a frame
    const int frames = parts.last().end() - parts.first().start;
    CHECK(std::abs(frames - length * kFps) <= 1);
    CHECK(std::abs(fit["residual_ms"].toInt()) <= 1000 / kFps);
    // Every cut on a bar one of the song, both sides
    const QJsonArray segments = fit["segments"].toArray();
    CHECK_EQ(segments.size(), parts.size());
    for (int i = 0; i + 1 < segments.size(); ++i) {
        int a = 0, b = 0;
        CHECK(onBar(segments[i]["to_s"].toDouble(), &a));
        CHECK(onBar(segments[i + 1]["from_s"].toDouble(), &b));
        if (phrases) CHECK(a % kPhraseBars == 0 && b % kPhraseBars == 0);
    }
    for (const QJsonValue& c : fit["cuts"].toArray())
        if (phrases) CHECK(c["phrase"].toBool());
    // The start kept (at most the silence before the first bar one), the last part = the song's own end
    CHECK(segments.first()["from_s"].toDouble() <= kLead + 0.01);
    CHECK(std::abs(segments.last()["to_s"].toDouble() - kSongLength) < 0.02);
    CHECK(segments.last()["from_s"].toDouble() <= barStart(kBars - kPhraseBars) + 0.01);
    CHECK_EQ(parts.last().out, p.mediaInfo(file)->length - 1);
    // The parts on one beat grid: the song time of every cut plays where the part before would have played it
    for (int i = 0; i + 1 < parts.size(); ++i) {
        const Clip &x = parts[i], &y = parts[i + 1];
        const double from = segments[i]["to_s"].toDouble() * kFps, to = segments[i + 1]["from_s"].toDouble() * kFps;
        const double before = x.start + x.subframe / 100.0 + (from - x.in);
        const double after = y.start + y.subframe / 100.0 + (to - y.in);
        if (!CHECK(std::abs(before - after) <= 0.012)) std::printf("       cut %d: %.3f vs %.3f frames\n", i, before, after);
        CHECK(x.transOut > 0 && y.transIn == x.transOut); // a crossfade
    }
    // No click: around the cuts the sound jumps no more than the song itself does anywhere
    const std::vector<float> sound = mix(p);
    for (const QJsonValue& c : fit["cuts_at"].toArray()) {
        const double at = c.toDouble(), jump = maxJump(sound, at - 0.1, at + 0.1);
        if (qEnvironmentVariableIsSet("JUMPS")) std::printf("       jump %.4f at %.2f s (song %.4f)\n", jump, at, baseline);
        if (!CHECK(jump <= baseline * 1.25 + 0.005)) std::printf("       jump %.4f at %.2f s (song %.4f)\n", jump, at, baseline);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("schneidi");

    const QString file = tmp.path() + "/song.wav";
    CHECK(writeWav(file, song()));
    const QString project = tmp.path() + "/p.schneidi";
    run("new", {{"project", project}, {"fps", kFps}});

    // The song as the analysis sees it
    const QJsonObject b = run("beats", {{"file", file}});
    CHECK(std::abs(b["bpm"].toDouble() - 120) < 0.5);
    CHECK(std::abs(b["downbeats"]["s"].toDouble() - kLead) < 0.02);
    CHECK_EQ(b["phrase_bars"].toInt(), kPhraseBars);

    // The song's own sample jumps (the same mixing path)
    edit(project, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"}])").arg(file));
    const std::vector<float> whole = mix(*load(project));
    const double baseline = maxJump(whole, 0, kSongLength);
    CHECK(whole.size() > size_t(100 * kRate));

    checkFit(project, file, 30, baseline, false);
    checkFit(project, file, 52, baseline, true); // 64 s = four phrases out of the middle

    // The drop (bar 32) on a climax: lands there within a bar
    {
        run("edit", {{"project", project}, {"ops", "[{\"op\":\"clear\"}]"}});
        const QJsonObject r = edit(project, QString(R"([{"op":"fit_music","media":"%1","length":"40s","climax_at":"12s"}])").arg(file));
        const QJsonObject fit = note(r, "fit_music").toObject();
        if (!CHECK(std::abs(fit["drop_at"].toDouble(-100) - 12) <= kBar))
            std::printf("       %s\n", QJsonDocument(fit).toJson(QJsonDocument::Compact).constData());
        const auto pPtr = load(project);
    const Project& p = *pPtr;
        int track = -1;
        for (int i = 0; i < p.timeline().audio.size(); ++i)
            if (!p.timeline().audio[i].clips.isEmpty()) track = i;
        const QVector<Clip> parts = clipsOn(p, std::max(0, track));
        CHECK(!parts.isEmpty() && std::abs(parts.last().end() - parts.first().start - 40 * kFps) <= 1);
    }
    // end:'fade': one part from the start, faded out
    {
        run("edit", {{"project", project}, {"ops", "[{\"op\":\"clear\"}]"}});
        const QJsonObject r = edit(project, QString(R"([{"op":"fit_music","media":"%1","length":"20s","end":"fade","track":"A1"}])").arg(file));
        const auto pPtr = load(project);
    const Project& p = *pPtr;
        const QVector<Clip> parts = clipsOn(p, 0);
        CHECK_EQ(parts.size(), 1);
        CHECK(!parts.isEmpty() && parts.first().length() == 20 * kFps && parts.first().fadeOut > 0 && parts.first().in == 0);
    }
    // Too short, bad arguments
    CHECK_EQ(edit(project, QString(R"([{"op":"fit_music","media":"%1","length":"200s"}])").arg(file), true)["failed"].toString(),
             QString("TOO_SHORT"));
    CHECK_EQ(edit(project, QString(R"([{"op":"fit_music","media":"%1","length":"20s","end":"loop"}])").arg(file), true)["failed"].toString(),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"fit_music","length":"20s"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));

    // A clip on the timeline replaced by its parts; one edit = one step back (restore)
    {
        run("edit", {{"project", project}, {"ops", "[{\"op\":\"clear\"}]"}});
        edit(project, QString(R"([{"op":"place","media":"%1","at":100,"track":"A2"}])").arg(file));
        const auto beforePtr = load(project);
    const Project& before = *beforePtr;
        const int id = clipsOn(before, 1).value(0).id;
        const QJsonObject r = edit(project, QString(R"([{"op":"fit_music","clip":%1,"length":"30s"}])").arg(id));
        const auto afterPtr = load(project);
    const Project& after = *afterPtr;
        const QVector<Clip> parts = clipsOn(after, 1);
        CHECK(parts.size() >= 2 && parts.first().start == 100);
        CHECK(!TimelineOps::findClip(after.timeline(), id));
        run("restore", {{"project", project}});
        const QVector<Clip> back = clipsOn(*load(project), 1);
        CHECK(back.size() == 1 && back.value(0).id == id && back.value(0).out == before.mediaInfo(file)->length - 1);
    }

    // snap_to_beat: a picture cut 0.28 s (14 frames) off the bar ones of the music on A2, rolled onto them
    {
        run("edit", {{"project", project}, {"ops", "[{\"op\":\"clear\"}]"}});
        edit(project, QString(R"([{"op":"place","media":"%1","at":0,"track":"A2"}])").arg(file));
        // Video clips of a picture-only file (no sound of their own)
        const QString video = tmp.path() + "/v.mp4";
        auto pPtr = load(project);
        pPtr->addMedia({video, "v.mp4", 20000, true, false, false});
        int ids[3] = {};
        pPtr->edit("pictures", [&](Timeline& tl) {
            const int cuts[4] = {0, 125 - 14, 225 + 14, 400};
            for (int i = 0; i < 3; ++i) {
                Clip c;
                c.id = ids[i] = pPtr->newClipId();
                c.mediaPath = video;
                c.start = cuts[i];
                c.in = 1000 + cuts[i];
                c.out = c.in + cuts[i + 1] - cuts[i] - 1;
                tl.video[0].clips << c;
            }
        });
        QString error;
        CHECK(ProjectFile::save(pPtr->data(), project, &error));
        // A beat away it stays: max_shift
        QJsonObject r = edit(project, R"([{"op":"snap_to_beat","grid":"bar","max_shift":"0.2s"}])");
        auto qPtr = load(project);
        CHECK_EQ(TimelineOps::findClip(qPtr->timeline(), ids[1])->start, 125 - 14);
        r = edit(project, R"([{"op":"snap_to_beat","grid":"bar","max_shift":"0.3s"}])");
        qPtr = load(project);
        CHECK_EQ(TimelineOps::findClip(qPtr->timeline(), ids[1])->start, 125);
        CHECK_EQ(TimelineOps::findClip(qPtr->timeline(), ids[2])->start, 225);
        CHECK_EQ(TimelineOps::findClip(qPtr->timeline(), ids[0])->end(), 125); // rolled: the clip before follows
        CHECK_EQ(TimelineOps::findClip(qPtr->timeline(), ids[2])->end(), 400);
        CHECK_EQ(note(r, "snap_to_beat").toArray().size(), 2);

        // A hit 0.2 s after a beat inside clip 2: its content slips, the accent keyframe goes with it
        pPtr = load(project);
        const Clip* c = TimelineOps::findClip(pPtr->timeline(), ids[1]);
        const int hitSource = c->in + (160 - c->start); // timeline 160, beats at 150 and 175
        pPtr->edit("accent", [&](Timeline& tl) {
            TimelineOps::changeClips(tl, [&](const Clip& x) { return x.id == ids[1]; },
                                     [&](Clip& x) { x.keys[AnimParam::ZoomX] = {{hitSource, 1.2}}; });
        });
        CHECK(ProjectFile::save(pPtr->data(), project, &error));
        r = edit(project, QString(R"([{"op":"snap_to_beat","event":"hit","events":[{"media":"%1","source":%2}]}])")
                              .arg(video)
                              .arg(hitSource));
        qPtr = load(project);
        c = TimelineOps::findClip(qPtr->timeline(), ids[1]);
        CHECK_EQ(c->start, 125); // the cut stays
        CHECK_EQ(c->start + (hitSource - c->in), 150);
        CHECK(c->keys.value(AnimParam::ZoomX).size() == 1 && c->keys.value(AnimParam::ZoomX).first().frame == hitSource);
        // A marker on clip 3 (5 frames after a beat) moves onto it with the content
        pPtr = load(project);
        pPtr->edit("marker", [&](Timeline& tl) { tl.markers = {305}; });
        CHECK(ProjectFile::save(pPtr->data(), project, &error));
        r = edit(project, R"([{"op":"snap_to_beat","event":"marker"}])");
        qPtr = load(project);
        CHECK(qPtr->timeline().markers == QVector<int>{300});
        c = TimelineOps::findClip(qPtr->timeline(), ids[2]);
        CHECK_EQ(c->in, 1000 + 225 + (305 - 225) - 75); // source of frame 305 (after the roll) now plays at 300
        CHECK_EQ(edit(project, R"([{"op":"snap_to_beat","grid":"bars"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
        // Two markers on one clip: the clip moves for the first, the second goes with its content (it stayed behind)
        pPtr = load(project);
        pPtr->edit("markers", [&](Timeline& tl) { tl.markers = {330, 360}; });
        CHECK(ProjectFile::save(pPtr->data(), project, &error));
        r = edit(project, R"([{"op":"snap_to_beat","event":"marker"}])");
        qPtr = load(project);
        if (!CHECK(qPtr->timeline().markers == (QVector<int>{325, 355})))
            for (int m : qPtr->timeline().markers) std::printf("       marker %d\n", m);
        // A locked track: nothing moves, the answer says so, the marker stays with its content
        pPtr = load(project);
        pPtr->edit("lock", [&](Timeline& tl) {
            tl.video[0].locked = true;
            tl.markers = {330};
        });
        CHECK(ProjectFile::save(pPtr->data(), project, &error));
        const int lockedStart = TimelineOps::findClip(pPtr->timeline(), ids[2])->start;
        r = edit(project, R"([{"op":"snap_to_beat","event":"marker","how":"move"}])");
        qPtr = load(project);
        CHECK_EQ(TimelineOps::findClip(qPtr->timeline(), ids[2])->start, lockedStart);
        CHECK(qPtr->timeline().markers == QVector<int>{330});
        const QJsonArray locked = note(r, "snap_to_beat").toArray();
        if (CHECK_EQ(locked.size(), 1)) {
            CHECK_EQ(locked[0]["shift_frames"].toInt(), 0);
            CHECK(locked[0]["note"].toString().contains("locked"));
        }
        // A clip with a gap before it is moved onto the beat - but not into the clip after it
        pPtr = load(project);
        pPtr->edit("room", [&](Timeline& tl) {
            tl.markers.clear();
            tl.video[0].locked = false;
            QVector<Clip>& v = tl.video[0].clips;
            v.resize(2);
            v[0].start = 120; // 5 frames before the beat at 125, a gap before it
            v[0].in = 1000;
            v[0].out = 1000 + 79; // up to 200
            v[1].start = 200;
            v[1].in = 2000;
            v[1].out = 2000 + 99;
        });
        CHECK(ProjectFile::save(pPtr->data(), project, &error));
        r = edit(project, R"([{"op":"snap_to_beat","clips":[)" + QByteArray::number(ids[0]) + R"(],"how":"move"}])");
        qPtr = load(project);
        if (CHECK_EQ(qPtr->timeline().video[0].clips.size(), 2)) {
            CHECK_EQ(qPtr->timeline().video[0].clips[1].start, 200); // the next clip keeps its start
            CHECK_EQ(qPtr->timeline().video[0].clips[1].in, 2000);
            CHECK_EQ(qPtr->timeline().video[0].clips[0].start, 120); // no room: stays
        }
    }

    // Live in the app: one undo step, the same as on the file
    {
        FakeApp fake;
        ProjectData d;
        d.timeline = emptyTimeline();
        d.format.rate = nearestFrameRate(kFps);
        fake.project.load(d);
        // Pictures cut 0.28 s off the bar ones (the music comes in the edits below)
        fake.project.addMedia({"/x/v.mp4", "v.mp4", 20000, true, false, false});
        fake.project.edit("pictures", [&](Timeline& tl) {
            const int cuts[4] = {0, 125 - 14, 225 + 14, 400};
            for (int i = 0; i < 3; ++i) {
                Clip c;
                c.id = fake.project.newClipId();
                c.mediaPath = "/x/v.mp4";
                c.start = cuts[i];
                c.in = 1000 + cuts[i];
                c.out = c.in + cuts[i + 1] - cuts[i] - 1;
                tl.video[0].clips << c;
            }
        });
        fake.path = tmp.path() + "/film.schneidi";
        QString error;
        CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
        fake.project.markSaved();
        const QString copy = tmp.path() + "/copy.schneidi";
        CHECK(QFile::copy(fake.path, copy));
        Cli::Live::Server server(&fake);
        if (!CHECK(server.start(&error))) return Check::result();
        QProcessEnvironment noLive = QProcessEnvironment::systemEnvironment();
        noLive.insert("XDG_DATA_HOME", tmp.path() + "/other");
        const QStringList calls{
            QString(R"([{"op":"place","media":"%1","at":0,"track":"A2"}])").arg(file),
            QString(R"([{"op":"fit_music","media":"%1","length":"30s","track":"A1"}])").arg(file),
            R"([{"op":"snap_to_beat","music":"A1","grid":"bar","max_shift":"0.3s"}])",
        };
        for (const QString& ops : calls) {
            const QByteArray before = state(fake.project);
            const int index = fake.project.undoStack()->index();
            const QJsonObject live = cli({"edit", fake.path, "--ops", ops});
            CHECK(live["live"].toBool());
            const QJsonObject onFile = cli({"edit", copy, "--ops", ops}, noLive);
            CHECK_EQ(fake.project.undoStack()->index(), index + 1);
            ProjectData fd;
            ProjectFile::load(copy, &fd, nullptr);
            Project fp;
            fp.load(fd);
            if (!CHECK(state(fp) == state(fake.project))) std::printf("       differs after %s\n", qPrintable(ops));
            if (ops.contains("\"place\"")) continue; // brings the song into the pool (not part of the undo)
            fake.project.undoStack()->undo();
            if (!CHECK(state(fake.project) == before)) std::printf("       undo differs after %s\n", qPrintable(ops));
            fake.project.undoStack()->redo();
        }
        // The cuts of the pictures on the bar ones of the cut song (its parts keep one grid)
        const Timeline& tl = fake.project.timeline();
        CHECK(tl.video[0].clips.size() == 3 && tl.video[0].clips[1].start == 125 && tl.video[0].clips[2].start == 225);
    }
    return Check::result();
}
