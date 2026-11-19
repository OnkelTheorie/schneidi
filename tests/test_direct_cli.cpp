// SPDX-License-Identifier: AGPL-3.0-only
// `direct` (0.25): recipe short and highlights suggest -> apply on synthetic recordings (lavfi picture, synthetic
// sounds): suggestions change nothing, apply is one edit with the project's "direction" set; material that is no game
// gives moments from laughter, cheering and movement; sessions by id or object, expired and stale ones refused; a
// canceled suggest or apply fails with CANCELED and leaves no half project. Prints the suggest time of 10 clips cold
// and warm (DIRECT_MEASURE=1 makes them 30 s long as in the plan's target).
#include "cli/Commands.h"
#include "core/ProjectFile.h"

#include "check.h"
#include "synth_sounds.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <atomic>
#include <thread>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw, const std::atomic<bool>* cancel = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    ctx.cancel = cancel;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"error", e.code}};
    }
}

// A "holiday clip": a moving picture with speech, laughter at ~10 s and cheering at 20-23 s
QString holiday(const QString& dir, const QString& name, double length, unsigned seed)
{
    const int rate = 48000;
    Synth::Builder b(rate, length, seed);
    Synth::speech(b, 1.5, 5.0);
    if (length > 12) Synth::laughter(b, 10.0);
    if (length > 24) Synth::cheering(b, 20.0, 3.0);
    b.floor();
    const QString wav = QDir(dir).filePath(name + ".wav");
    if (!Synth::writeWav(wav, b.s, rate)) return {};
    return Check::makeMedia(QDir(dir).filePath(name + ".mp4"),
                            {"-f", "lavfi", "-i", QString("testsrc2=s=640x360:r=30:d=%1").arg(length), "-i", wav,
                             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
}

QStringList labelsOf(const QJsonArray& suggestions)
{
    QStringList all;
    for (const QJsonValue& s : suggestions)
        for (const QJsonValue& l : s["labels"].toArray()) all << l.toString();
    for (const QJsonValue& s : suggestions) all << s["reason"]["key"].toString();
    return all;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("direct_cli");
    Check::initEngine();
    QTemporaryDir dir;

    // ---- Without material: the errors of the contract
    CHECK_EQ(run("direct", {{"recipe", "talk"}, {"files", QJsonArray{}}})["error"].toString(), QString("UNKNOWN_RECIPE"));
    CHECK_EQ(run("direct", {{"recipe", "dance"}})["error"].toString(), QString("UNKNOWN_RECIPE"));
    CHECK_EQ(run("direct", {{"recipe", "short"}})["error"].toString(), QString("NO_MATERIAL"));
    CHECK_EQ(run("direct", {{"session", "d999"}, {"apply", QJsonObject{{"on", "all"}}}})["error"].toString(),
             QString("SESSION_EXPIRED"));
    if (!Check::haveFfmpeg()) return Check::result();

    // ---- Recipe short on a "game" recording with two loud moments (folder named like a game's, game none)
    QDir().mkpath(dir.filePath("clips/cs2"));
    const QString clip = Check::makeMedia(
        dir.filePath("clips/cs2/cs2_clip_test.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=40", "-f", "lavfi", "-i",
         "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,9,9.6),0.4*sin(2*PI*440*t),0)+if(between(t,29,29.6),0.9*sin(2*PI*440*t),0)':d=40:s=48000",
         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!clip.isEmpty())) return Check::result();
    const QJsonObject suggested = run("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}},
                                                 {"options", QJsonObject{{"game", "none"}, {"variants", 2}}}});
    const QJsonObject session = suggested["session"].toObject();
    CHECK(session["id"].toString().startsWith("d"));
    CHECK(suggested["next"].toString().contains("apply"));
    CHECK(!QFileInfo::exists(dir.filePath("clips/cs2/cs2_clip_test-short.schneidi"))); // suggesting changes nothing
    const QJsonArray suggestions = session["suggestions"].toArray();
    int on = 0;
    bool both = false, loud = true;
    for (const QJsonValue& s : suggestions) {
        if (!s["on"].toBool()) continue;
        ++on;
        CHECK_EQ(s["variant"].toInt(), 1);
        loud &= s["reason"]["key"].toString() == "loud";
        CHECK(!s["reason"]["text"].toString().isEmpty());
        both |= s["from_s"].toDouble() <= 9.3 && s["to_s"].toDouble() >= 29.3;
    }
    CHECK(on >= 1 && loud);
    CHECK_EQ(session["material"][0]["has_video"].toBool(), true);
    CHECK_EQ(session["material"][0]["analysis"]["sound"].toString(), QString("done"));
    CHECK(session["length_s"].toDouble() >= 14.9 && session["length_s"].toDouble() <= 45.1);
    CHECK_EQ(session["order"].toArray().size(), on);
    Q_UNUSED(both);

    // dry_run and plan need a project; a plan changes nothing
    const QString proj = dir.filePath("out/short.schneidi");
    QDir().mkpath(dir.filePath("out"));
    run("new", {{"project", proj}, {"media", QJsonArray{clip}}});
    const QJsonObject planned = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                               {"project", proj}, {"plan", true}});
    CHECK(planned["plan"]["id"].toString().startsWith("p"));
    CHECK_EQ(run("info", {{"project", proj}, {"summary", true}})["timelines"].toArray().size(), 1);
    const QJsonObject dry = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                           {"project", proj}, {"dry_run", true}});
    CHECK(dry["dry_run"].toBool());
    CHECK_EQ(run("info", {{"project", proj}, {"summary", true}})["timelines"].toArray().size(), 1);
    // Apply: one edit, the cut and the 9:16 copy, the project remembers the session
    const QJsonObject applied = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                               {"project", proj}});
    CHECK(!applied.contains("error"));
    CHECK_EQ(applied["timeline"].toString(), QString("Short"));
    CHECK_EQ(applied["cut_timeline"].toString(), QString("Short-Schnitt"));
    CHECK_EQ(applied["moments"].toArray().size(), on);
    CHECK(applied["check"].toObject().contains("issues"));
    {
        const QJsonObject info = run("info", {{"project", proj}, {"timeline", "Short"}, {"summary", true}});
        CHECK_EQ(info["timeline"]["height"].toInt(), 1920);
        CHECK(std::abs(info["timeline"]["length_s"].toDouble() - applied["length_s"].toDouble()) < 0.1);
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(proj, &d, &error));
        CHECK_EQ(d.direction["id"].toString(), session["id"].toString());
        CHECK_EQ(d.direction["recipe"].toString(), QString("short"));
    }
    // A choice of its own: only the first suggestion, moved edges, as an object (the page sends what it shows)
    {
        QJsonObject changed = session;
        const int n = suggestions[0]["n"].toInt();
        const QJsonObject one = run("direct", {{"session", changed}, {"project", proj},
                                               {"apply", QJsonObject{{"on", QJsonArray{n}},
                                                                     {"adjust", QJsonArray{QJsonObject{{"n", n}, {"from_s", 2.0}, {"to_s", 7.5}}}}}}});
        CHECK_EQ(one["moments"].toArray().size(), 1);
        CHECK_EQ(one["length_s"].toDouble(), 5.5);
        CHECK_EQ(one["timeline"].toString(), QString("Short 2"));
        // Expired (made two days ago) and a list naming no suggestion
        changed["made"] = QDateTime::currentDateTimeUtc().addDays(-2).toString(Qt::ISODate);
        CHECK_EQ(run("direct", {{"session", changed}, {"apply", QJsonObject{{"on", "all"}}}, {"project", proj}})["error"].toString(),
                 QString("SESSION_EXPIRED"));
        CHECK_EQ(run("direct", {{"session", session}, {"apply", QJsonObject{{"on", QJsonArray{99}}}}, {"project", proj}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
    }
    // Stale: the recording changed since the session was made
    {
        const QString copy = dir.filePath("clips/copy.mp4");
        CHECK(QFile::copy(clip, copy));
        const QJsonObject s = run("direct", {{"recipe", "short"}, {"files", QJsonArray{copy}}})["session"].toObject();
        QFile f(copy);
        CHECK(f.open(QIODevice::Append));
        f.write("x");
        f.close();
        CHECK_EQ(run("direct", {{"session", s["id"]}, {"apply", QJsonObject{{"on", "all"}}}, {"project", proj}})["error"].toString(),
                 QString("SESSION_STALE"));
    }

    // ---- Recipe highlights on material that is no game: laughter, cheering and movement give moments
    const QString beach = holiday(dir.path(), "beach", 30, 3);
    if (!CHECK(!beach.isEmpty())) return Check::result();
    {
        const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{beach}},
                                             {"options", QJsonObject{{"length_s", "all"}, {"order", "time"}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        const QStringList labels = labelsOf(list);
        if (!CHECK(labels.contains("laughter") || labels.contains("cheering")))
            std::printf("       %s\n", QJsonDocument(list).toJson().constData());
        CHECK(labels.contains("motion") || labels.contains("scene_change") || labels.contains("loud"));
        // In time order
        double last = -1;
        for (const QJsonValue& v : r["session"]["order"].toArray()) {
            for (const QJsonValue& s : list)
                if (s["n"].toInt() == v.toInt()) {
                    CHECK(s["from_s"].toDouble() > last);
                    last = s["from_s"].toDouble();
                }
        }
        const QJsonObject made = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        CHECK(!made.contains("error"));
        CHECK_EQ(made["timeline"].toString(), QString("Highlights"));
        CHECK(QFileInfo::exists(made["project"].toString())); // a new project next to the recording
        // A second recipe on the same material reads the caches
        QElapsedTimer t;
        t.start();
        const QJsonObject again = run("direct", {{"recipe", "short"}, {"files", QJsonArray{beach}}});
        CHECK(!again.contains("error"));
        std::printf("       second recipe on the same clip: %lld ms\n", t.elapsed());
    }

    // ---- Cancel: a running suggest stops with CANCELED; an apply canceled before it starts leaves no project
    {
        const QString fresh = holiday(dir.path(), "fresh", 30, 9);
        std::atomic<bool> stop{false};
        std::thread canceler([&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop = true;
        });
        const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{fresh}}}, &stop);
        canceler.join();
        CHECK_EQ(r["error"].toString(), QString("CANCELED"));
        const QJsonObject s = run("direct", {{"recipe", "short"}, {"files", QJsonArray{beach}}})["session"].toObject();
        std::atomic<bool> already{true};
        const QString target = dir.filePath("out/never.schneidi");
        const QJsonObject a = run("direct", {{"session", s["id"]}, {"apply", QJsonObject{{"on", "all"}}}}, &already);
        CHECK_EQ(a["error"].toString(), QString("CANCELED"));
        CHECK(!QFileInfo::exists(QFileInfo(beach).absolutePath() + "/beach-short.schneidi"));
        Q_UNUSED(target);
    }

    // ---- Tempo: suggestions for 10 clips, cold and warm
    {
        const double len = qEnvironmentVariableIntValue("DIRECT_MEASURE") ? 30 : 12;
        QJsonArray ten;
        for (int i = 0; i < 10; ++i) ten << holiday(dir.path(), QString("ten%1").arg(i), len, 20 + i);
        QElapsedTimer t;
        t.start();
        const QJsonObject cold = run("direct", {{"recipe", "highlights"}, {"files", ten}});
        const qint64 coldMs = t.restart();
        const QJsonObject warm = run("direct", {{"recipe", "highlights"}, {"files", ten}});
        const qint64 warmMs = t.elapsed();
        CHECK(!cold.contains("error") && !warm.contains("error"));
        CHECK(warmMs * 3 < coldMs + 300); // the caches make it much quicker
        std::printf("       10 clips of %.0f s: cold %lld ms, warm %lld ms\n", len, coldMs, warmMs);
    }
    return Check::result();
}
