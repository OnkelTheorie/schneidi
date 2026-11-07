// SPDX-License-Identifier: AGPL-3.0-only
// Review render (0.17, `render review:"video"|"sheet"`) on a synthetic project: a recording with a shot in its sound
// and a kill feed line, two plain clips after it, a 100 BPM click track on A2. The marks are burned in at the right
// frames (lamps sampled at the event's frame and at a frame without one), a normal export stays without them, the
// project file is not touched, offsets/cuts_off_beat give the known numbers (hit 0.28 s after a beat, a cut 0.2 s
// off), the sheet has one tile per shot showing that shot, highlights come from the cache the second time, and the
// parameters are in the command's schema.
#include "cli/Commands.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ReviewSheet.h"

#include "render_check.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"error", e.code}};
    }
}

QByteArray fileHash(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha1);
}

// Mean colour of a rectangle's inner part
QColor meanColor(const QImage& img, QRect r)
{
    r = r.adjusted(r.width() / 4, r.height() / 4, -r.width() / 4, -r.height() / 4);
    long long sr = 0, sg = 0, sb = 0, n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            sr += c.red();
            sg += c.green();
            sb += c.blue();
            ++n;
        }
    if (n == 0) return {};
    return QColor(int(sr / n), int(sg / n), int(sb / n));
}

bool isLit(const QColor& c, const QColor& lamp)
{
    return std::abs(c.red() - lamp.red()) < 50 && std::abs(c.green() - lamp.green()) < 50 && std::abs(c.blue() - lamp.blue()) < 50;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("review_render");
    Check::initEngine();
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    QTemporaryDir dir;
    using namespace ReviewSheet;

    // ---- Pure parts: nearest beat, offsets, cuts off the beat, lamps
    {
        Data d;
        d.format = {640, 360, {25, 1}};
        d.length = 200;
        for (int k = 0; k < 13; ++k) {
            d.beats.push_back(k * 0.6);
            d.bar.push_back(char(k % 4 == 0));
        }
        d.marks << Mark{MarkKind::Hit, 112, "rec", 1} << Mark{MarkKind::Cut, 125, "green", 2}
                << Mark{MarkKind::Cut, 150, "blue", 3} << Mark{MarkKind::Hit, 190, "far", 4};
        d.sort();
        CHECK_EQ(nearestBeat(d, 4.48), 7);
        CHECK_EQ(nearestBeat(d, 4.55), 8);
        const QVector<Offset> o = offsets(d);
        if (CHECK_EQ(o.size(), 2)) {
            CHECK_EQ(o[0].offMs, 280);
            CHECK(std::abs(o[0].beat - 4.2) < 1e-9);
            CHECK_EQ(o[0].clip, 1);
            CHECK_EQ(o[1].offMs, 400); // 7.6 s next to the last beat 7.2 s, within one period after it
        }
        const QVector<Offset> cuts = cutsOffBeat(d);
        if (CHECK_EQ(cuts.size(), 1)) { // 5.0 s is 0.2 s after 4.8; 6.0 s is a beat
            CHECK_EQ(cuts[0].frame, 125);
            CHECK_EQ(cuts[0].offMs, 200);
        }
        CHECK_EQ(lampHold(25), 3);
        CHECK(lampLit(d, Lamp::Hit, 112) && lampLit(d, Lamp::Hit, 114) && !lampLit(d, Lamp::Hit, 115)
              && !lampLit(d, Lamp::Hit, 111));
        CHECK(lampLit(d, Lamp::Beat, 105) && !lampLit(d, Lamp::Beat, 103));
        CHECK(lampLit(d, Lamp::Bar, 120) && !lampLit(d, Lamp::Bar, 105)); // beat 8 = bar one, beat 7 not
        CHECK(!lampRect(Lamp::Hit, {960, 540}).intersects(stripRect({960, 540})));
        CHECK(QRect(QPoint(), QSize(960, 540)).contains(lampRect(Lamp::Kill, {960, 540})));
    }

    // ---- Synthetic project: recording (shot at 4.44 s, kill feed line from 4.6 s), green, blue; click track on A2
    const QString rec = Check::makeMedia(
        dir.filePath("rec.mkv"),
        {"-f", "lavfi", "-i",
         "color=c=0x202020:s=640x360:d=6:r=25,"
         "drawbox=x=400:y=20:w=20:h=14:c=0xd01010:t=fill:enable='gte(t,4.60)',"
         "drawbox=x=400:y=20:w=50:h=14:c=0xd01010:t=fill:enable='gte(t,4.72)',"
         "drawbox=x=400:y=20:w=120:h=14:c=0xd01010:t=fill:enable='gte(t,4.80)'",
         "-f", "lavfi", "-i",
         "aevalsrc='0.01*(random(0)-0.5)+if(between(t,4.44,4.49),0.8*sin(2*PI*900*t),0)':s=48000:d=6",
         "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", "-shortest"});
    const QString green = Check::makeMedia(dir.filePath("green.mkv"), {"-f", "lavfi", "-i", "color=c=0x10c020:s=640x360:d=2:r=25",
                                                                        "-c:v", "ffv1", "-pix_fmt", "bgr0"});
    const QString blue = Check::makeMedia(dir.filePath("blue.mkv"), {"-f", "lavfi", "-i", "color=c=0x1030e0:s=640x360:d=3:r=25",
                                                                      "-c:v", "ffv1", "-pix_fmt", "bgr0"});
    // 100 BPM: a click every 0.6 s from 0, the bar ones with a kick under them
    const QString song = Check::makeMedia(
        dir.filePath("song.wav"),
        {"-f", "lavfi", "-i",
         "aevalsrc='if(lt(mod(t,0.6),0.03),0.6*sin(2*PI*1500*t)*exp(-mod(t,0.6)*80),0)"
         "+if(lt(mod(t,2.4),0.12),0.8*sin(2*PI*55*t)*exp(-mod(t,2.4)*25),0)':s=48000:d=8",
         "-c:a", "pcm_s16le"});
    if (rec.isEmpty() || green.isEmpty() || blue.isEmpty() || song.isEmpty()) return Check::skip("ffmpeg could not make the media");
    const QString project = dir.filePath("review.schneidi");
    CHECK(!run("new", {{"project", project}, {"width", 640}, {"height", 360}, {"fps", 25},
                       {"media", QJsonArray{rec, green, blue, song}}})
               .contains("error"));
    const QJsonArray ops{
        QJsonObject{{"op", "append"}, {"media", rec}, {"in", 0}, {"out", 125}},
        QJsonObject{{"op", "append"}, {"media", green}, {"in", 0}, {"out", 25}},
        QJsonObject{{"op", "append"}, {"media", blue}, {"in", 0}, {"out", 50}},
        QJsonObject{{"op", "place"}, {"media", song}, {"at", 0}, {"track", "A2"}, {"in", 0}, {"out", 200}},
    };
    const QJsonObject edited = run("edit", {{"project", project}, {"ops", ops}});
    CHECK(!edited.contains("error"));
    // A section, an open and a done note, a marker (written into the file)
    {
        ProjectData d;
        QString error;
        if (CHECK(ProjectFile::load(project, &d, &error))) {
            d.timeline.sections = {{StoryKind::Hook, "Opening", 0, 50}, {StoryKind::Build, {}, 50, 200}};
            d.timeline.notes = {{1, 75, "faster here", false}, {2, 80, "done already", true}};
            d.timeline.markers = {60};
            CHECK(ProjectFile::save(d, project, &error));
        }
    }
    const QByteArray before = fileHash(project);
    const QDateTime beforeTime = QFileInfo(project).lastModified();
    const QStringList filesBefore = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    const QJsonObject hitsArg{{"region", "0.5,0,0.5,0.4"}, {"color", "#d01010"}};

    // ---- review video: marks at the right frames
    const QString out = dir.filePath("review.mp4");
    const QJsonObject r = run("render", {{"project", project}, {"out", out}, {"review", "video"},
                                         {"review_hits", hitsArg}, {"encoder", "software"}});
    if (qEnvironmentVariableIsSet("REVIEW_JSON")) {
        std::printf("%s\n", QJsonDocument(r).toJson().constData());
        std::printf("%s\n", QJsonDocument(run("info", {{"project", project}})).toJson().constData());
    }
    CHECK_EQ(r["review"].toString(), QString("video"));
    CHECK_EQ(r["size"].toArray(), (QJsonArray{960, 540})); // 540 p by default
    CHECK_EQ(r["beats"]["track"].toString(), QString("A2"));
    CHECK(std::abs(r["beats"]["bpm"].toDouble() - 100) < 1);
    CHECK_EQ(r["marks"]["hits"].toInt(), 1);
    CHECK_EQ(r["marks"]["kills"].toInt(), 1);
    CHECK_EQ(r["marks"]["cuts"].toInt(), 2);
    CHECK_EQ(r["marks"]["notes"].toInt(), 1); // the done one is left out
    CHECK_EQ(r["marks"]["markers"].toInt(), 1);
    CHECK_EQ(r["marks"]["sections"].toInt(), 2);
    // The hit as highlights finds it (the clip plays the recording from its start: timeline = source time)
    QJsonObject hl = hitsArg;
    hl["file"] = rec;
    const QJsonObject hlr = run("highlights", hl);
    CHECK(hlr["cached"].toBool()); // the review stored it in the analysis cache
    const double hitS = hlr["candidates"][0]["hit_s"][0].toDouble();
    CHECK(std::abs(hitS - 4.44) <= 0.041);
    int hitFrame = -1;
    if (const QJsonArray offs = r["offsets"].toArray(); CHECK_EQ(offs.size(), 1)) {
        const QJsonObject o = offs[0].toObject();
        hitFrame = o["frame"].toInt();
        CHECK_EQ(hitFrame, int(std::lround(hitS * 25)));
        CHECK(std::abs(o["hit_s"].toDouble() - hitS) < 0.001);
        CHECK(std::abs(o["beat_s"].toDouble() - 4.2) < 0.02); // beat 7 of the 100 BPM grid
        CHECK(std::abs(o["off_ms"].toInt() - std::lround((hitS - 4.2) * 1000)) <= 15);
        CHECK_EQ(o["name"].toString(), QString("rec.mkv"));
        if (!CHECK(o["clip"].toInt() > 0)) std::printf("       offsets: %s\n", QJsonDocument(offs).toJson().constData());
    }
    if (const QJsonArray cuts = r["cuts_off_beat"].toArray(); CHECK_EQ(cuts.size(), 1)) {
        CHECK_EQ(cuts[0]["frame"].toInt(), 125);
        CHECK(std::abs(cuts[0]["off_ms"].toInt() - 200) <= 20);
        CHECK_EQ(cuts[0]["name"].toString(), QString("green.mkv"));
    }
    const ProjectFormat of{960, 540, {25, 1}};
    const QSize os(960, 540);
    const auto lamp = [&](Lamp l, int frame) {
        const QImage img = Check::mediaFrame(out, of, frame);
        return img.isNull() ? QColor() : meanColor(img, lampRect(l, os));
    };
    if (CHECK(QFileInfo::exists(out)) && hitFrame > 0) {
        const QColor hitOn = lamp(Lamp::Hit, hitFrame), hitOff = lamp(Lamp::Hit, hitFrame - 2);
        CHECK(isLit(hitOn, lampColor(Lamp::Hit)));
        CHECK(!isLit(hitOff, lampColor(Lamp::Hit)));
        // Beat 7 at 4.2 s = frame 105; 103 has none
        CHECK(isLit(lamp(Lamp::Beat, 105), lampColor(Lamp::Beat)));
        CHECK(!isLit(lamp(Lamp::Beat, 103), lampColor(Lamp::Beat)));
        // The cut to the green clip at frame 125
        CHECK(isLit(lamp(Lamp::Cut, 125), lampColor(Lamp::Cut)));
        CHECK(!isLit(lamp(Lamp::Cut, 122), lampColor(Lamp::Cut)));
        std::printf("       hit lamp on %s off %s\n", qPrintable(hitOn.name()), qPrintable(hitOff.name()));
        // The strip at the bottom: darker than the clip under it, a picture above it unchanged (green at 5.5 s)
        const QImage g = Check::mediaFrame(out, of, 137);
        if (CHECK(!g.isNull())) {
            const QColor mid = meanColor(g, QRect(380, 220, 200, 100));
            CHECK(mid.green() > 150 && mid.red() < 60);
            const QRect strip = stripRect(os);
            CHECK(meanColor(g, QRect(20, strip.top() + strip.height() / 2 - 4, 60, 8)).green() < 110);
        }
    }

    // ---- The project is not changed (no save, no backup, no new file next to it besides the render)
    CHECK_EQ(fileHash(project), before);
    CHECK(QFileInfo(project).lastModified() == beforeTime);
    QStringList filesAfter = QDir(dir.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    filesAfter.removeAll("review.mp4");
    CHECK_EQ(filesAfter, filesBefore);

    // ---- A normal export has no overlay: the lamp place shows the clip
    {
        const QString plain = dir.filePath("plain.mp4");
        const QJsonObject n = run("render", {{"project", project}, {"out", plain}, {"height", 540}, {"encoder", "software"}});
        CHECK(!n.contains("review") && !n.contains("offsets"));
        const QImage img = Check::mediaFrame(plain, of, 112);
        if (CHECK(!img.isNull())) {
            const QColor c = meanColor(img, lampRect(Lamp::Hit, os));
            CHECK(std::abs(c.red() - 0x20) < 12 && std::abs(c.green() - 0x20) < 12);
        }
    }

    CHECK_EQ(r["hits_from"][0]["file"].toString(), rec);

    // ---- review sheet: one tile per shot, showing it
    {
        const QString png = dir.filePath("sheet.png");
        const QJsonObject sh = run("render", {{"project", project}, {"out", png}, {"review", "sheet"}, {"review_hits", hitsArg}});
        CHECK_EQ(sh["review"].toString(), QString("sheet"));
        CHECK_EQ(sh["offsets"].toArray().size(), 1);
        const QImage img(png);
        if (const QString dump = qEnvironmentVariable("REVIEW_DUMP"); !dump.isEmpty()) {
            img.save(dump + "/sheet.png");
            Check::mediaFrame(out, of, hitFrame).save(dump + "/hit.png");
        }
        if (CHECK(!img.isNull())) {
            CHECK_EQ(img.width(), kSheetWidth);
            const QJsonArray tiles = sh["tiles"].toArray();
            if (CHECK_EQ(tiles.size(), 3)) {
                CHECK_EQ(tiles[1]["clip"].toString(), QString("green.mkv"));
                CHECK(std::abs(tiles[1]["from_s"].toDouble() - 5.0) < 1e-6);
                const auto rectOf = [&](int i) {
                    const QJsonArray a = tiles[i]["rect"].toArray();
                    return QRect(a[0].toInt(), a[1].toInt(), a[2].toInt(), a[3].toInt());
                };
                const QColor t1 = meanColor(img, rectOf(1)), t2 = meanColor(img, rectOf(2)), t0 = meanColor(img, rectOf(0));
                CHECK(t1.green() > 150 && t1.blue() < 80);
                CHECK(t2.blue() > 170 && t2.green() < 90);
                CHECK(t0.red() < 60 && t0.green() < 60);
            }
        }
        CHECK(sh["size"][1].toInt() > 300);
        CHECK_EQ(fileHash(project), before);
        // Not a picture extension
        CHECK_EQ(run("render", {{"project", project}, {"out", dir.filePath("s.mp4")}, {"review", "sheet"}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
        CHECK_EQ(run("render", {{"project", project}, {"out", dir.filePath("x.mp4")}, {"review", "nope"}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
    }

    // ---- Without a music track: no beat grid, a note; review_hits off: no hits
    {
        const QJsonObject n = run("render", {{"project", project}, {"out", dir.filePath("nobeat.png")}, {"review", "sheet"},
                                             {"beats_track", "off"}, {"review_hits", false}});
        CHECK(!n.contains("beats"));
        CHECK_EQ(n["marks"]["hits"].toInt(), 0);
        CHECK_EQ(n["offsets"].toArray().size(), 0);
    }

    // ---- Documented in the schema
    {
        const Cli::Command* cmd = Cli::find("render");
        const QJsonObject props = Cli::inputSchema(*cmd)["properties"].toObject();
        for (const char* k : {"review", "review_hits", "beats_track"}) CHECK(props.contains(k));
        CHECK(props["review"]["description"].toString().contains("offsets"));
        CHECK(props["review"]["description"].toString().contains("sheet"));
    }
    return Check::result();
}
