// SPDX-License-Identifier: AGPL-3.0-only
// B-roll to what is said (0.17 package "broll"): the edit op broll searches the material index (`index`, its shots
// described with annotate) with each spoken sentence of the timeline and puts the best shot onto V2 over the
// sentence - picture only, never from the file playing below, no shot twice; a sentence without a match is named.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/MediaIndex.h"

#include "check.h"

#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <memory>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& project, const QJsonArray& ops)
{
    try {
        run("edit", {{"project", project}, {"ops", ops}});
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

TranscriptWord word(double from, const QString& text) { return {qint64(from * 1000), qint64(from * 1000 + 350), text}; }

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("broll");
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir dir;
    const QString talk = Check::makeMedia(QDir(dir.path()).filePath("talk.mp4"),
                                          {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=12", "-f", "lavfi", "-i",
                                           "sine=frequency=300:duration=12", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                                           "-c:a", "aac", "-shortest"});
    const QString shop = Check::makeMedia(QDir(dir.path()).filePath("shop.mp4"),
                                          {"-f", "lavfi", "-i", "smptebars=s=320x180:r=25:d=6", "-f", "lavfi", "-i",
                                           "sine=frequency=500:duration=6", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                                           "-c:a", "aac", "-shortest"});
    if (!CHECK(!talk.isEmpty() && !shop.isEmpty())) return Check::result();
    const QString project = QDir(dir.path()).filePath("broll.schneidi");
    run("new", {{"project", project}, {"media", QJsonArray{talk, shop}}});
    // The talk on V1, with its transcript: two sentences about the shop, one about nothing in the material
    {
        ProjectData d;
        ProjectFile::load(project, &d, nullptr);
        Project p;
        p.load(d);
        p.setTranscript(talk, MediaTranscript{0, "de", {word(0.5, "Heute"), word(0.9, "sind"), word(1.3, "wir"),
                                                        word(1.7, "im"), word(2.1, "Laden."), word(4.0, "Das"),
                                                        word(4.4, "Wetter"), word(4.8, "ist"), word(5.2, "schön."),
                                                        word(7.0, "Noch"), word(7.4, "mal"), word(7.8, "der"),
                                                        word(8.2, "Laden.")}});
        QString error;
        CHECK(ProjectFile::save(p.data(), project, &error));
    }
    run("edit", {{"project", project},
                 {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", talk}}}}});
    // No index yet in the cache of this test: broll builds it (on a file), but nothing is described
    MediaIndex::clearMemoryCache();
    QJsonObject r = run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "broll"}}}}});
    QString answer = QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
    CHECK(answer.contains("\"placed\":[]") && answer.contains("no_match"));
    // The shop described
    run("index", {{"files", QJsonArray{shop}}});
    run("index", {{"annotate", QJsonArray{QJsonObject{{"file", shop}, {"shot", 1}, {"desc", "Der Laden von außen"},
                                                      {"tags", QJsonArray{"laden", "totale"}}}}}});
    r = run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "broll"}}}}});
    answer = QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact));
    CHECK(answer.contains("shop.mp4"));
    CHECK(answer.contains("Das Wetter ist schön.")); // no match for it
    CHECK(answer.contains("Noch mal der Laden.")); // the only shop shot is used once already
    {
        ProjectData d;
        ProjectFile::load(project, &d, nullptr);
        Project p;
        p.load(d);
        const Timeline& tl = p.timeline();
        if (CHECK(tl.video.size() >= 2) && CHECK_EQ(tl.video[1].clips.size(), 1)) {
            const Clip& c = tl.video[1].clips[0];
            CHECK_EQ(c.mediaPath, shop);
            CHECK(std::abs(c.start - 12) <= 1); // the first sentence: 0.5 s
            CHECK(c.length() >= 40 && c.length() <= 50); // as long as it is said (0.5 .. 2.45 s)
        }
        int shopSound = 0;
        for (const Track& t : tl.audio)
            for (const Clip& c : t.clips) shopSound += c.mediaPath == shop;
        CHECK_EQ(shopSound, 0); // picture only
    }
    // A query for the whole range; and the file playing below is never its own B-roll
    r = run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "broll"}, {"query", "laden"}, {"from", "6s"}}}}});
    CHECK(QJsonDocument(r).toJson().contains("shop.mp4"));
    CHECK(errorOf(project, {QJsonObject{{"op", "broll"}, {"track", "V1"}}}).startsWith("BAD_ARGUMENT"));
    return Check::result();
}
