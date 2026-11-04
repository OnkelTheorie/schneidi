// SPDX-License-Identifier: AGPL-3.0-only
// J/L cuts and room tone (0.17): Editor::soundEdge/setSoundEdge/splitEdit roll only the sound edge of a cut (picture,
// links and the rest stay in sync, one undo step, limits of the material), the edit op jl_cut on a project file
// (at|clip, J and L, errors with how far it can go) and room_tone (a loop of the take's quietest stretch fills the
// gaps of a dialogue track at the take's floor, without clicks at seams and edges).
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RoomTone.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include "check.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>

namespace {

constexpr int kRate = 48000;

Clip media(int id, int start, int in, int length, int link, int stream = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.mp4";
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    c.linkId = link;
    c.audioStream = stream;
    return c;
}

// V1: 1 [0,100) (file 200..299: nothing after it) | 2 [100,200) (file 20..119) | 3 [200,300) (file 0..99)
// A1: 4, 5, 6 linked to them; A2: 7, 8 (second stream of 1 and 2); A3: music 9 (not linked)
void setup(Project& p)
{
    p.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 300, true, true, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(3);
    tl.video[0].clips << media(1, 0, 200, 100, 1) << media(2, 100, 20, 100, 2) << media(3, 200, 0, 100, 3);
    tl.audio[0].clips << media(4, 0, 200, 100, 1) << media(5, 100, 20, 100, 2) << media(6, 200, 0, 100, 3);
    tl.audio[1].clips << media(7, 0, 200, 100, 1, 1) << media(8, 100, 20, 100, 2, 1);
    tl.audio[2].clips << media(9, 0, 0, 300, 0);
    p.load(ProjectData{ProjectFormat{}, 0, p.media(), {}, tl, 9, 3});
}

Clip clipOf(const Project& p, int id)
{
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    return c ? *c : Clip{};
}

QByteArray state(const Project& p)
{
    ProjectData d = p.data();
    d.lastClipId = d.lastLinkId = d.lastSequenceId = 0;
    return ProjectFile::toJson(d, "/x/t.schneidi");
}

void testSoundEdge()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    setup(p);
    const Timeline before = p.timeline();
    const QByteArray start = state(p);

    // Cut 1|2: the sound of 2 can start 20 frames early (its file has 20 before), 1 has nothing after: no L
    const auto e = ed.soundEdge(1, 2);
    if (!CHECK(e.has_value())) return;
    CHECK_EQ(e->pairs.size(), 2); // A1 and A2
    CHECK_EQ(e->cut, 100);
    CHECK_EQ(e->offset, 0);
    CHECK_EQ(e->lo, -20);
    CHECK_EQ(e->hi, 0);
    CHECK(!ed.soundEdge(1, 3)); // not a cut

    // J cut by 10 frames: one undo step, only the sound edges move
    const int index = p.undoStack()->index();
    CHECK(ed.setSoundEdge(1, 2, -10));
    CHECK_EQ(p.undoStack()->index(), index + 1);
    CHECK_EQ(clipOf(p, 4).end(), 90);
    CHECK_EQ(clipOf(p, 5).start, 90);
    CHECK_EQ(clipOf(p, 5).in, 10); // the same file frame stays at the same time: in sync
    CHECK_EQ(clipOf(p, 5).end(), 200);
    CHECK_EQ(clipOf(p, 7).end(), 90);
    CHECK_EQ(clipOf(p, 8).start, 90);
    CHECK_EQ(clipOf(p, 8).in, 10);
    for (int id : {1, 2, 3, 6, 9}) // picture, the next cut and the music unchanged
        if (!CHECK(clipOf(p, id) == *TimelineOps::findClip(before, id))) std::printf("       clip %d changed\n", id);
    for (int id : {4, 5, 7, 8}) CHECK_EQ(clipOf(p, id).linkId, TimelineOps::findClip(before, id)->linkId);
    CHECK_EQ(ed.soundEdge(1, 2)->offset, -10);

    // Beyond the material: nothing happens
    CHECK(!ed.setSoundEdge(1, 2, -21));
    CHECK(!ed.setSoundEdge(1, 2, 1));
    CHECK_EQ(p.undoStack()->index(), index + 1);
    // Back in sync with 0, and undo gives back the start
    CHECK(ed.setSoundEdge(1, 2, 0));
    CHECK(p.timeline() == before);
    p.undoStack()->undo();
    p.undoStack()->undo();
    CHECK(state(p) == start);

    // L cut at 2|3: the sound of 2 runs on 12 frames under the picture of 3 (3 has no material before: no J)
    const auto l = ed.soundEdge(2, 3);
    if (CHECK(l.has_value())) {
        CHECK_EQ(l->pairs.size(), 1); // only A1 has sound of both
        CHECK_EQ(l->lo, 0);
        CHECK_EQ(l->hi, 99); // the sound of 3 keeps one frame
    }
    CHECK(ed.setSoundEdge(2, 3, 12));
    CHECK_EQ(clipOf(p, 5).end(), 212);
    CHECK_EQ(clipOf(p, 5).out, 131);
    CHECK_EQ(clipOf(p, 6).start, 212);
    CHECK_EQ(clipOf(p, 6).in, 12);
    CHECK_EQ(clipOf(p, 2).end(), 200);
    CHECK_EQ(clipOf(p, 3).start, 200);
    p.undoStack()->undo();

    // Locked sound track: not rolled (A1 locked -> only A2 at 1|2)
    ed.toggleTrackLock({TrackKind::Audio, 0});
    CHECK_EQ(ed.soundEdge(1, 2)->pairs.size(), 1);
    ed.toggleTrackLock({TrackKind::Audio, 0});

    // Split edit (Alt-drag at a cut): the cut of one track only
    const TimelineOps::TrimEdit split = ed.splitEdit(5, TimelineOps::Edge::Start);
    CHECK(split.kind == TimelineOps::TrimKind::Roll);
    CHECK(split.ids == QVector<int>{4});
    CHECK(split.rightIds == QVector<int>{5});
    CHECK_EQ(ed.clampTrimEdit(split, -50), -20);
    ed.applyTrimEdit(split, -5);
    CHECK_EQ(clipOf(p, 4).end(), 95);
    CHECK_EQ(clipOf(p, 5).start, 95);
    CHECK_EQ(clipOf(p, 7).end(), 100); // A2 and the picture keep their cut
    CHECK_EQ(clipOf(p, 1).end(), 100);
    CHECK_EQ(clipOf(p, 2).start, 100);
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

QJsonObject op(std::initializer_list<QPair<QString, QJsonValue>> kv)
{
    QJsonObject o;
    for (const auto& [k, v] : kv) o[k] = v;
    return o;
}

void testJlCutOp(const QString& dir)
{
    g_path = dir + "/jl.schneidi";
    {
        Project p;
        setup(p);
        QString error;
        if (!CHECK(ProjectFile::save(p.data(), g_path, &error))) return;
    }
    // J by time near the cut: the nearest picture cut with linked sound (1|2); -0.4 s = 10 frames at 25 fps
    QJsonObject r = run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "jl_cut"}, {"at", 97}, {"audio", "-0.4s"}})}}});
    CHECK(QJsonDocument(r).toJson().contains("J cut"));
    {
        const auto own = loaded();
        const Project& p = *own;
        CHECK_EQ(clipOf(p, 4).end(), 90);
        CHECK_EQ(clipOf(p, 5).start, 90);
        CHECK_EQ(clipOf(p, 8).in, 10);
        CHECK_EQ(clipOf(p, 1).end(), 100);
        CHECK_EQ(clipOf(p, 2).start, 100);
    }
    // audio is measured from the picture cut: -5 now means 5 early, not 15
    run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "jl_cut"}, {"clip", 2}, {"audio", -5}})}}});
    CHECK_EQ(clipOf(*loaded(), 5).start, 95);
    // L by clip: the sound clip 6 stands for its picture 3, cut at its start (2|3)
    run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "jl_cut"}, {"clip", 6}, {"audio", "+12"}})}}});
    {
        const auto own = loaded();
        const Project& p = *own;
        CHECK_EQ(clipOf(p, 5).end(), 212);
        CHECK_EQ(clipOf(p, 6).start, 212);
        CHECK_EQ(clipOf(p, 3).start, 200);
        CHECK_EQ(clipOf(p, 5).start, 95); // the J at the other end stays
    }
    // edge:'end' = the clip before the cut
    run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "jl_cut"}, {"clip", 2}, {"edge", "end"}, {"audio", 0}})}}});
    CHECK_EQ(clipOf(*loaded(), 6).start, 200);

    // Errors: beyond the material (says how far it goes), no cut there, missing audio
    const QString far = errorOf({op({{"op", "jl_cut"}, {"clip", 2}, {"audio", -25}})});
    CHECK(far.startsWith("BAD_ARGUMENT"));
    if (!CHECK(far.contains("20 frames") && far.contains("up to 0 frames"))) std::printf("       %s\n", qPrintable(far));
    CHECK(errorOf({op({{"op", "jl_cut"}, {"clip", 1}, {"audio", 5}})}).startsWith("FAILED")); // nothing before clip 1
    CHECK(errorOf({op({{"op", "jl_cut"}, {"clip", 2}})}).startsWith("BAD_ARGUMENT"));
    // A number is frames like everywhere: a fraction (meant as seconds) says how to write them, a huge one is refused
    // (it was rounded to 1 frame / cut to int: a silent wrong J/L cut)
    CHECK(errorOf({op({{"op", "jl_cut"}, {"clip", 2}, {"audio", -0.5}})}).contains("\"0.5s\""));
    CHECK(errorOf({op({{"op", "jl_cut"}, {"clip", 2}, {"audio", 4294967296.0}})}).startsWith("BAD_ARGUMENT"));
    CHECK(errorOf({op({{"op", "jl_cut"}, {"clip", 9}, {"audio", 5}})}).startsWith("BAD_ARGUMENT")); // music: no picture
    CHECK_EQ(clipOf(*loaded(), 5).start, 95); // errors changed nothing
}

// ---------- room tone ----------

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

// 40 s take: a 125 Hz hum (5 periods a frame: pieces of it meet in phase, so a seam shows only what the cut does) at -50 dBFS under everything (the room), "words" (loud noise) at 0-4, 6-10, 12.5-20 and
// 22-38 s, and a stretch where the room is quieter (-56 dBFS) at 10-12.5 s
std::vector<float> take()
{
    std::vector<float> s(size_t(40 * kRate));
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> noise(-0.1f, 0.1f);
    const double hum = std::sqrt(2.0) * std::pow(10.0, -50 / 20.0);
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i) / kRate;
        const bool quiet = t >= 10 && t < 12.5;
        const bool words = t < 4 || (t >= 6 && t < 10) || (t >= 12.5 && t < 20) || (t >= 22 && t < 38);
        s[i] = float((quiet ? hum / 2 : hum) * std::sin(2 * M_PI * 125 * t)) + (words ? noise(rng) : 0.f);
    }
    return s;
}

// Channel 0 of the timeline mix [from, to)
std::vector<float> mix(const Project& p, int from, int to)
{
    std::vector<float> out;
    AudioAnalysis::timelineSound(p.format(), p.timeline(), from, to, [&](const float* pcm, int n, int, int) {
        if (pcm) out.insert(out.end(), pcm, pcm + n);
        else out.insert(out.end(), size_t(n), 0.f);
    });
    return out;
}

double rmsDb(const std::vector<float>& s, size_t from, size_t to)
{
    double sum = 0;
    for (size_t i = from; i < to; ++i) sum += double(s[i]) * s[i];
    return 10 * std::log10(sum / double(std::max<size_t>(1, to - from)) + 1e-30);
}

// Envelope of the hum: the largest |sample| of each half period (any such window holds one crest), every 1 ms
std::vector<double> envelope(const std::vector<float>& s, size_t from, size_t to)
{
    constexpr size_t half = kRate / 125 / 2, hop = kRate / 1000;
    std::vector<double> e;
    for (size_t i = from; i + half <= to; i += hop) {
        double m = 0;
        for (size_t j = i; j < i + half; ++j) m = std::max(m, double(std::abs(s[j])));
        e.push_back(m);
    }
    return e;
}

void testRoomTone(const QString& dir)
{
    // The pure part: floor and quiet stretch
    {
        // Words, then room broken by words every 12 blocks, a quieter stretch with a click, digital silence and
        // the only 15 blocks of room in one piece
        std::vector<double> db(260, -50.0);
        for (int i = 0; i < 100; ++i) db[i] = -20;
        for (int i = 111; i < 200; i += 12) db[i] = -20;
        for (int i = 200; i < 217; ++i) db[i] = -58;
        db[208] = -40;
        for (int i = 217; i < 245; ++i) db[i] = -200;
        RoomTone::Quiet q = RoomTone::findQuiet(db, 20, 10);
        CHECK_EQ(q.floorDb, -50.0);
        CHECK(q.found());
        CHECK_EQ(q.from, 245); // 20 fits nowhere, 15 only there
        CHECK_EQ(q.to, 260);
        CHECK(std::abs(q.meanDb + 50) < 1e-6);
        // A steady quieter stretch long enough wins over the louder room; one that steps in level does not
        for (int i = 200; i < 217; ++i) db[i] = -58;
        for (int i = 217; i < 245; ++i) db[i] = -57;
        q = RoomTone::findQuiet(db, 20, 10);
        CHECK(q.found() && q.from >= 200 && q.to <= 245 && q.meanDb < -56);
        for (int i = 217; i < 245; ++i) db[i] = -50; // -58 then -50: a step of 8 dB, the -58 alone is too short
        q = RoomTone::findQuiet(db, 20, 20);
        CHECK(q.found() && std::abs(q.meanDb + 50) < 1e-6);
        CHECK(!RoomTone::findQuiet(std::vector<double>(50, -20.0), 100, 60).found());
        CHECK_EQ(RoomTone::seamFrames(25), 1);
        CHECK_EQ(RoomTone::seamFrames(120), 2);
    }

    const QString wav = dir + "/talk.wav";
    if (!CHECK(writeWav(wav, take()))) return;
    g_path = dir + "/talk.schneidi";
    run("new", {{"project", g_path}, {"width", 320}, {"height", 180}, {"fps", 25}});
    // Two pieces of the take with a 3 s gap (4.4 .. 7.4 s) between; both end/start in the room (no words)
    run("edit", {{"project", g_path},
                 {"ops", QJsonArray{op({{"op", "place"}, {"media", wav}, {"at", 0}, {"in", 0}, {"out", "4.4s"}}),
                                    op({{"op", "place"}, {"media", wav}, {"at", "7.4s"}, {"in", "5.6s"}, {"out", "10s"}})}}});
    const auto before = loaded();
    if (!CHECK_EQ(before->timeline().audio[0].clips.size(), 2)) return;
    const int g0 = before->timeline().audio[0].clips[0].end(), g1 = before->timeline().audio[0].clips[1].start;
    CHECK_EQ(g0, 110);
    CHECK_EQ(g1, 185);

    const QJsonObject r = run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "room_tone"}, {"track", "A1"}})}}});
    const QByteArray answer = QJsonDocument(r).toJson();
    CHECK(answer.contains("room_tone: A1, 1 gap"));
    const auto own = loaded();
        const Project& p = *own;
    const auto& clips = p.timeline().audio[0].clips;
    if (!CHECK(clips.size() >= 3)) {
        std::printf("       %s\n", answer.constData());
        return;
    }
    // The gap is filled exactly, from the quiet stretch (10..12.5 s = frames 250..312), louder by about 6 dB
    int covered = 0;
    for (const Clip& c : clips) {
        if (c.start < g0 || c.end() > g1) continue;
        covered += c.length();
        CHECK(c.in >= 250 && c.out + 1 + RoomTone::seamFrames(25) <= 313);
        CHECK(std::abs(c.volumeDb - 6.0) < 0.6);
        CHECK_EQ(c.linkId, 0);
    }
    CHECK_EQ(covered, g1 - g0);
    // The takes' sound before/after the cut stays: a dissolve that starts at / ends on the cut
    CHECK_EQ(clips.first().transOut, RoomTone::seamFrames(25));
    CHECK(clips.first().transOutStyle.align == TransitionAlign::Start);
    CHECK(clips.last().transInStyle.align == TransitionAlign::End);

    // In the mix: the gap sounds like the room around it, and no seam clicks
    const int from = g0 - 10, to = g1 + 10; // 4.0 .. 7.8 s: all room (no words)
    const std::vector<float> s = mix(p, from, to);
    const size_t perFrame = kRate / 25;
    if (!CHECK_EQ(s.size(), size_t(to - from) * perFrame)) return;
    const double room = rmsDb(s, 0, 10 * perFrame);          // the take before the gap
    const double gap = rmsDb(s, 12 * perFrame, (g1 - from - 2) * perFrame);
    if (!CHECK(std::abs(gap - room) < 1.5)) std::printf("       room %.1f dB, gap %.1f dB\n", room, gap);
    // No click (a crest above the room) and no drop (the mixer's declick ramp at a hard cut dips to zero) at the
    // seams and the gap's edges
    std::vector<double> ref = envelope(s, 0, 9 * perFrame);
    std::sort(ref.begin(), ref.end());
    const double crest = ref[ref.size() / 2];
    const std::vector<double> seams = envelope(s, 9 * perFrame, s.size());
    const auto [lo, hi] = std::minmax_element(seams.begin(), seams.end());
    if (!CHECK(*lo > 0.7 * crest && *hi < 1.5 * crest))
        std::printf("       envelope %.5f..%.5f at the seams, %.5f in the room\n", *lo, *hi, crest);
    // Before: silence in the gap (the drop the op is for)
    const std::vector<float> dry = mix(*before, from, to);
    CHECK(rmsDb(dry, 12 * perFrame, (g1 - from - 2) * perFrame) < -90);

    // Again: no gaps left, nothing changes
    const QByteArray filled = state(*loaded());
    run("edit", {{"project", g_path}, {"ops", QJsonArray{op({{"op", "room_tone"}, {"track", "A1"}})}}});
    CHECK(state(*loaded()) == filled);
    // Errors
    CHECK(errorOf({op({{"op", "room_tone"}, {"track", "V1"}})}).startsWith("BAD_ARGUMENT"));
    CHECK(errorOf({op({{"op", "room_tone"}})}).startsWith("BAD_ARGUMENT"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("jl-cuts");
    QTemporaryDir dir;
    testSoundEdge();
    testJlCutOp(dir.path());
    testRoomTone(dir.path());
    return Check::result();
}
