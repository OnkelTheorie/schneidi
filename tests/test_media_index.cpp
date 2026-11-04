// SPDX-License-Identifier: AGPL-3.0-only
// Material index (engine/MediaIndex, core/IndexSearch, cli `index`): shots and quality values of a synthetic clip made
// of a sharp, a blurred, a dark, a bright, a panning and a skin-coloured shot; the second call comes from the cache
// (and a changed file is measured anew); annotate writes outside the cache and survives clearing it; search finds by
// tag, description, transcript and (stubbed) hearable event, also with German synonyms ("lacht" -> laughter,
// "dunkel"); hits in timeline time for trimmed clips; a file name with umlauts; the MCP description stays short.
#include "cli/Commands.h"
#include "core/IndexSearch.h"
#include "engine/MediaIndex.h"

#include "check.h"

#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw, Cli::Context* given = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    if (!cmd) return {{"failed", "NO_COMMAND"}};
    Cli::Context own;
    Cli::Context& ctx = given ? *given : own;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return QJsonObject{{"failed", e.code}};
    }
}

QJsonObject shotAt(const QJsonArray& shots, double at)
{
    for (const QJsonValue& v : shots) {
        const QJsonObject o = v.toObject();
        if (o["from_s"].toDouble() <= at && at < o["to_s"].toDouble()) return o;
    }
    return {};
}

void searchOnly()
{
    // core/IndexSearch alone: synonyms, part words, filler words, events narrow the hit
    QVector<IndexSearch::Shot> shots(4);
    shots[0] = {"a.mp4", 1, 0, 4, "ruhige Totale vom Laden", {"totale", "laden"}, {}, {}, 0.2};
    shots[1] = {"a.mp4", 2, 4, 9, {}, {}, "und dann sind wir zur Ladentheke gegangen", {}, 4.0};
    shots[2] = {"b.mp4", 1, 0, 6, {}, {}, {}, {{"laughter", 2.5, 3.4, 0.8}}, 3.0};
    shots[3] = {"b.mp4", 2, 6, 9, {}, {}, {}, {}, 2.0};
    shots[3].dark = true;
    CHECK_EQ(IndexSearch::queryWords("Wo lacht er?"), QStringList{"lacht"});
    auto hits = IndexSearch::search("wo lacht er", shots);
    if (CHECK_EQ(hits.size(), 1)) {
        CHECK_EQ(hits[0].file, QString("b.mp4"));
        CHECK(std::abs(hits[0].from - 2.5) < 1e-9 && std::abs(hits[0].to - 3.4) < 1e-9); // the event, not the shot
        CHECK(hits[0].why.contains("event laughter 0.80"));
    }
    hits = IndexSearch::search("Gelächter", shots);
    CHECK(hits.size() == 1 && hits[0].file == "b.mp4");
    hits = IndexSearch::search("laughing", shots);
    CHECK(hits.size() == 1 && hits[0].file == "b.mp4");
    hits = IndexSearch::search("ruhige Totale vom Laden", shots);
    if (CHECK(hits.size() >= 2)) {
        CHECK_EQ(hits[0].n, 1); // all three words in tags/desc, quality calm too
        CHECK(hits[0].why.contains("tag 'totale'"));
        CHECK_EQ(hits[1].n, 2); // "Ladentheke" in the speech: part word
        CHECK(hits[1].why.contains("speech"));
    }
    hits = IndexSearch::search("wide shot", shots); // English for "Totale"
    CHECK(!hits.isEmpty() && hits[0].n == 1 && hits[0].file == "a.mp4");
    hits = IndexSearch::search("dunkel", shots);
    CHECK(hits.size() == 1 && hits[0].file == "b.mp4" && hits[0].n == 2 && hits[0].why == "quality dark");
    CHECK(IndexSearch::search("Applaus", shots).isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("media_index");
    searchOnly();
    // The cache key names how the audio events are made: installing or updating the audio-events model (YAMNet)
    // measures anew instead of keeping the heuristic events (pure part: a real model needs the ONNX Runtime extension)
    CHECK_EQ(AudioEvents::wayKey({}), QString("heuristic"));
    CHECK(AudioEvents::wayKey("/m/yamnet.onnx").startsWith("yamnet|"));
    CHECK(MediaIndex::cacheKey("/x/a.mp4", AudioEvents::wayKey({}))
          != MediaIndex::cacheKey("/x/a.mp4", AudioEvents::wayKey("/m/yamnet.onnx")));
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg missing");
    Check::initEngine();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8());
    QDir(MediaIndex::cacheDir()).removeRecursively();
    QDir(MediaIndex::notesDir()).removeRecursively();
    CHECK(!MediaIndex::notesDir().startsWith(MediaIndex::cacheDir())); // written text is not cache

    // Six shots of 2 s each, every one different enough from the one before for a cut: sharp still, blurred,
    // dark, bright, a pan, a skin-coloured area. Umlauts in the name (index key on Windows).
    const QString base = "testsrc=s=1280x720:r=25:d=2";
    const QString crop = "crop=640:360:0:0";
    const QString media = Check::makeMedia(
        dir.filePath("Läden ü.mp4"),
        {"-f", "lavfi", "-i", base, "-f", "lavfi", "-i", base, "-f", "lavfi", "-i", base, "-f", "lavfi", "-i", base,
         "-f", "lavfi", "-i", base, "-f", "lavfi", "-i", base, "-f", "lavfi", "-i", "sine=f=440:d=12", "-filter_complex",
         QString("[0]%1[s0];[1]%1,negate,gblur=sigma=10[s1];[2]%1,lutyuv=y=val*0.12[s2];"
                 "[3]%1,negate,lutyuv=y=215+val*0.15[s3];[4]crop=640:360:'t*120':0[s4];"
                 "[5]%1,negate,drawbox=x=160:y=80:w=320:h=200:color=0xE0AC69:t=fill[s5];"
                 "[s0][s1][s2][s3][s4][s5]concat=n=6:v=1:a=0[v]")
             .arg(crop),
         "-map", "[v]", "-map", "6:a", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!media.isEmpty())) return Check::result();

    // Stubbed hearable events (engine/AudioEvents comes from package C): a laugh in the bright shot
    int eventCalls = 0;
    MediaIndex::setEventSource([&eventCalls](const QString&) {
        ++eventCalls;
        return QVector<AudioEvents::Event>{{"laughter", 6.4, 7.2, 0.8}, {"applause", 10.5, 11.5, 0.6}};
    });

    QElapsedTimer timer;
    timer.start();
    QJsonObject r = run("index", {{"files", QJsonArray{media}}});
    const qint64 firstMs = timer.elapsed();
    const QJsonArray mediaList = r["media"].toArray();
    if (!CHECK_EQ(mediaList.size(), 1)) return Check::result();
    CHECK_EQ(r["cached"].toBool(true), false);
    const QJsonArray shots = mediaList[0].toObject()["shots"].toArray();
    for (const QJsonValue& v : shots)
        std::printf("shot %s\n", QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact).constData());
    CHECK_EQ(shots.size(), 6);
    if (shots.size() == 6) {
        for (int i = 0; i < 6; ++i) CHECK(std::abs(shots[i].toObject()["from_s"].toDouble() - 2.0 * i) < 0.1);
        const auto q = [&](int i) { return shots[i].toObject()["quality"].toObject(); };
        CHECK(!q(0)["blurry"].toBool() && q(0)["sharp"].toDouble() > 0.6);
        CHECK(q(0)["motion"].toDouble() < 1.0 && !q(0)["pan"].toBool());
        CHECK(q(1)["blurry"].toBool() && q(1)["sharp"].toDouble() < q(0)["sharp"].toDouble());
        CHECK(q(2)["dark"].toBool() && !q(0)["dark"].toBool());
        CHECK(q(3)["overexposed"].toBool() && !q(3)["dark"].toBool());
        CHECK(q(4)["pan"].toBool() && q(4)["motion"].toDouble() > 2.0);
        CHECK_EQ(q(5)["face"].toString(), QString("likely"));
        CHECK(q(0)["face"].toString() != QLatin1String("likely"));
        CHECK(shots[3].toObject()["events"].toArray().size() == 1);
        CHECK(!shots[0].toObject().contains("events"));
    }
    CHECK(r["note"].toString().contains("annotate"));

    // Second call: from the cache, no new measurement and no events asked again
    timer.restart();
    r = run("index", {{"files", QJsonArray{media}}});
    const qint64 secondMs = timer.elapsed();
    std::printf("first %lld ms, cached %lld ms\n", static_cast<long long>(firstMs), static_cast<long long>(secondMs));
    CHECK(r["cached"].toBool());
    CHECK_EQ(eventCalls, 1);
    CHECK(secondMs * 20 < std::max<qint64>(firstMs, 100)); // < 5 %
    MediaIndex::clearMemoryCache();
    r = run("index", {{"files", QJsonArray{media}}});
    CHECK(r["cached"].toBool()); // from disk
    CHECK_EQ(eventCalls, 1);

    // MCP: the undescribed shots come as a sheet to look at
    Cli::Context mcp;
    mcp.mcp = true;
    r = run("index", {{"file", media}}, &mcp);
    CHECK_EQ(mcp.images.size(), 1);
    CHECK_EQ(r["sheet"].toArray().size(), 6);

    // annotate -> query finds it by tag and description; synonyms in both languages; an event via "lacht"
    r = run("index", {{"annotate", QJsonArray{QJsonObject{{"file", media}, {"shot", 1}, {"desc", "ruhige Totale vom Laden"},
                                                          {"tags", QJsonArray{"totale", "laden"}}},
                                              QJsonObject{{"file", media}, {"shot", 5}, {"tags", "auto, straße"}}}}});
    CHECK_EQ(r["annotated"].toArray().size(), 2);
    CHECK(QFileInfo::exists(MediaIndex::notesDir()));
    const auto hitsOf = [&](const QString& query, const QJsonObject& more = {}) {
        QJsonObject a{{"files", QJsonArray{media}}, {"query", query}};
        for (auto it = more.begin(); it != more.end(); ++it) a[it.key()] = it.value();
        return run("index", a)["hits"].toArray();
    };
    QJsonArray hits = hitsOf("ruhige Totale vom Laden");
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 1);
    hits = hitsOf("Straße");
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 5 && hits[0].toObject()["why"].toString().startsWith("tag"));
    hits = hitsOf("wo lacht er");
    if (CHECK(!hits.isEmpty())) {
        CHECK_EQ(hits[0].toObject()["shot"].toInt(), 4);
        CHECK(std::abs(hits[0].toObject()["from_s"].toDouble() - 6.4) < 1e-6);
        CHECK(hits[0].toObject()["why"].toString().contains("laughter"));
    }
    hits = hitsOf("Applaus");
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 6);
    hits = hitsOf("dunkel");
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 3);
    hits = hitsOf("Kameraschwenk"); // part word: "schwenk"
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 5);
    hits = hitsOf("unscharf");
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 2);

    // Clearing the cache keeps what was written (measured again, the notes go back onto the same shots)
    QDir(MediaIndex::cacheDir()).removeRecursively();
    MediaIndex::clearMemoryCache();
    hits = hitsOf("Totale");
    CHECK(!hits.isEmpty() && hits[0].toObject()["shot"].toInt() == 1);
    CHECK_EQ(eventCalls, 2); // measured anew
    // A changed file (new modification time) is measured anew too
    QFile touch(media);
    if (touch.open(QIODevice::ReadWrite)) {
        touch.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime);
        touch.close();
    }
    MediaIndex::clearMemoryCache();
    r = run("index", {{"files", QJsonArray{media}}});
    CHECK(!r["cached"].toBool(true));
    CHECK_EQ(eventCalls, 3);

    // A moved project folder (or another drive letter): the notes follow the file by its name, size and first MB
    {
        const QString moved = dir.filePath("moved/") + QFileInfo(media).fileName();
        QDir().mkpath(QFileInfo(moved).absolutePath());
        CHECK(QFile::copy(media, moved));
        CHECK_EQ(MediaIndex::notes(moved).size(), 2);
        const QJsonArray found = run("index", {{"files", QJsonArray{moved}}, {"query", "ruhige Totale"}})["hits"].toArray();
        CHECK(!found.isEmpty() && found[0].toObject()["shot"].toInt() == 1);
        // Same name, other content: not the same file
        const QString other = dir.filePath("other/") + QFileInfo(media).fileName();
        QDir().mkpath(QFileInfo(other).absolutePath());
        QFile o(other);
        if (CHECK(o.open(QIODevice::WriteOnly))) o.write(QByteArray(int(QFileInfo(media).size()), 'x'));
        o.close();
        CHECK(MediaIndex::notes(other).isEmpty());
    }

    // Project: transcript words become the shots' speech; hits in timeline time where the trimmed clips play
    const QString project = dir.filePath("p.schneidi");
    CHECK(!run("new", {{"project", project}, {"template", "neutral"}, {"fps", 25}, {"media", QJsonArray{media}}})
               .contains("failed"));
    // source 4-6 s (dark shot) at 0, source 8-10 s (pan) at 2 s; the blurred shot is not on the timeline
    CHECK(!run("edit", {{"project", project},
                        {"ops", QJsonArray{QJsonObject{{"op", "keep"}, {"media", media},
                                                       {"ranges", QJsonArray{QJsonArray{"4s", "6s"}, QJsonArray{"8s", "10s"}}}}}}})
               .contains("failed"));
    {
        QFile f(project);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonArray list = o["media"].toArray();
        for (int i = 0; i < list.size(); ++i) {
            QJsonObject m = list[i].toObject();
            m["transcripts"] = QJsonArray{QJsonObject{
                {"stream", 0}, {"language", "de"},
                {"words", QJsonArray{QJsonArray{8300, 8700, "wir"}, QJsonArray{8700, 9200, "fahren"}, QJsonArray{9200, 9800, "Fahrrad"}}}}};
            list[i] = m;
        }
        o["media"] = list;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(o).toJson());
    }
    hits = run("index", {{"project", project}, {"query", "Fahrrad"}})["hits"].toArray();
    if (CHECK_EQ(hits.size(), 1)) {
        const QJsonObject h = hits[0].toObject();
        CHECK_EQ(h["shot"].toInt(), 5);
        CHECK(h["why"].toString().startsWith("speech"));
        const QJsonArray at = h["timeline"].toArray();
        if (CHECK_EQ(at.size(), 1)) { // the video clip, not its linked sound again
            CHECK_EQ(at[0].toObject()["from"].toInt(), 50); // source 8 s plays at 2 s
            CHECK_EQ(at[0].toObject()["to"].toInt(), 100);
            CHECK_EQ(at[0].toObject()["track"].toString(), QString("V1"));
        }
    }
    hits = run("index", {{"project", project}, {"query", "dunkel"}})["hits"].toArray();
    if (CHECK(!hits.isEmpty())) {
        const QJsonArray at = hits[0].toObject()["timeline"].toArray();
        CHECK(at.size() == 1 && at[0].toObject()["from"].toInt() == 0 && at[0].toObject()["to"].toInt() == 50);
    }
    hits = run("index", {{"project", project}, {"query", "unscharf"}})["hits"].toArray();
    CHECK(!hits.isEmpty() && !hits[0].toObject().contains("timeline")); // not on the timeline

    // Thumbnails of two files with the same name in different folders: each has its own (they overwrote each other)
    {
        QDir().mkpath(dir.filePath("b"));
        const QString twin = dir.filePath("b/Läden ü.mp4");
        CHECK(QFile::copy(media, twin));
        const QJsonArray both = run("index", {{"files", QJsonArray{media, twin}}, {"thumbs", dir.filePath("thumbs")}})["media"].toArray();
        if (CHECK_EQ(both.size(), 2)) {
            const QString a = both[0]["shots"][0]["thumb"].toString(), b = both[1]["shots"][0]["thumb"].toString();
            CHECK(!a.isEmpty() && !b.isEmpty() && a != b);
            CHECK(QFileInfo::exists(a) && QFileInfo::exists(b));
        }
    }

    // Words, not letters: "lacht" is no "flache Landschaft", "Stille" is silence and not every calm shot
    {
        IndexSearch::Shot flat;
        flat.file = "/x/a.mp4";
        flat.n = 1;
        flat.to = 2;
        flat.motion = 3;
        flat.desc = "flache Landschaft";
        IndexSearch::Shot calm = flat;
        calm.n = 2;
        calm.desc = "Totale";
        calm.motion = 0.2;
        IndexSearch::Shot laugh = flat;
        laugh.n = 3;
        laugh.desc = "sie lacht laut";
        IndexSearch::Shot roof = flat;
        roof.n = 4;
        roof.desc = "Kameraschwenk";
        const QVector<IndexSearch::Shot> shots{flat, calm, laugh, roof};
        const QVector<IndexSearch::Hit> l = IndexSearch::search("wo lacht sie", shots);
        if (CHECK_EQ(l.size(), 1)) CHECK_EQ(l[0].n, 3);
        CHECK(IndexSearch::search("Stille", shots).isEmpty());
        const QVector<IndexSearch::Hit> pan = IndexSearch::search("Schwenk", shots); // a compound's head still counts
        if (CHECK_EQ(pan.size(), 1)) CHECK_EQ(pan[0].n, 4);
    }

    // Errors: an unknown shot, a missing file
    CHECK_EQ(run("index", {{"annotate", QJsonArray{QJsonObject{{"file", media}, {"shot", 40}, {"desc", "x"}}}}})["failed"].toString(),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(run("index", {{"file", dir.filePath("nope.mp4")}})["failed"].toString(), QString("NOT_FOUND"));

    // The MCP description stays short (every tool costs the AI context)
    const Cli::Command* cmd = Cli::find("index");
    if (CHECK(cmd)) {
        std::printf("index description %lld chars\n", static_cast<long long>(cmd->summary.size()));
        CHECK(cmd->summary.size() < 700);
        CHECK(cmd->params.size() <= 10);
    }
    MediaIndex::setEventSource({});

    // Cancelled (the app closes while the Media Pool builds the index): a sound-only file stops too, nothing is
    // cached (the audio part never asked, closing the app waited for the whole file)
    {
        const QString podcast = Check::makeMedia(dir.filePath("podcast.wav"), {"-f", "lavfi", "-i", "sine=f=300:d=60"});
        if (CHECK(!podcast.isEmpty())) {
            MediaIndex::Options o;
            int asked = 0;
            o.progress = [&asked](double) {
                ++asked;
                return false;
            };
            QString error;
            CHECK(!MediaIndex::build(podcast, o, &error).has_value());
            CHECK(asked > 0);
            CHECK(!MediaIndex::cached(podcast).has_value());
            CHECK(MediaIndex::build(podcast).has_value()); // not cancelled: built
        }
    }
    return Check::result();
}
