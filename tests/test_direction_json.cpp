// SPDX-License-Identifier: AGPL-3.0-only
// The Direct session (core/Direction): JSON there and back, the fixed reason keys with English and UI texts, the order
// of the switched-on suggestions; the project key "direction" (written only when set, old projects load, set without
// an undo step but changed) and that it is not part of a plan's base (an open plan stays valid).
#include "cli/Commands.h"
#include "core/Direction.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include "check.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

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

Direction::Session example()
{
    Direction::Session s;
    s.id = "d3";
    s.recipe = "short";
    s.made = "2026-11-19T12:40:00Z";
    s.options = {{"for", "shorts"}, {"variants", 2}};
    s.project = "/x/p.schneidi";
    Direction::Material m;
    m.file = "/clips/a.mkv";
    m.length = 31.2;
    m.hasVideo = m.hasAudio = true;
    m.analysis = {{"sound", "done"}, {"picture", "done"}, {"events", "running"}, {"speech", "none"}};
    m.size = 1234;
    m.modified = 99;
    m.fps = 60;
    m.width = 1920;
    m.height = 1080;
    m.audioStreams = {"Mix", "Game"};
    m.moments = 4;
    s.material << m;
    Direction::Suggestion a;
    a.n = 1;
    a.file = m.file;
    a.from = 12.4;
    a.to = 19;
    a.score = 31.5;
    a.reason = {"kills", {{"n", 2}, {"extra", "multi"}}};
    a.labels = {"kill", "loud"};
    a.variant = 1;
    Direction::Suggestion b = a;
    b.n = 2;
    b.from = 25;
    b.to = 31.2;
    b.reason = {"laughter", {{"s", 2.1}}};
    b.labels = {"laughter"};
    b.on = false;
    b.variant = 2;
    Direction::Suggestion c = a;
    c.n = 3;
    c.kind = "pause";
    c.track = "A1";
    c.reason = {"pause", {{"s", 0.8}}};
    c.variant = 0;
    s.suggestions << a << b << c;
    s.order = {3, 1};
    s.target = 45;
    s.notes << Direction::Note{"no_whisper", Direction::noteText("no_whisper")};
    return s;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("direction_json");
    Check::initEngine();

    // ---- There and back: the same session; the reason text is added for the AI and ignored when read
    {
        const Direction::Session s = example();
        const QJsonObject j = Direction::toJson(s);
        CHECK_EQ(j["suggestions"][0]["reason"]["text"].toString(), QString("2 kills (multi kill)"));
        CHECK_EQ(j["suggestions"][1]["reason"]["text"].toString(), QString("laughter (2.1 s)"));
        CHECK_EQ(j["suggestions"][1]["variant"].toInt(), 2);
        CHECK(!j["suggestions"][2].toObject().contains("variant"));
        CHECK_EQ(j["suggestions"][2]["track"].toString(), QString("A1"));
        CHECK_EQ(j["length_s"].toDouble(), 6.6 + 6.6); // the two switched on
        CHECK_EQ(j["target_s"].toDouble(), 45.0);
        const auto back = Direction::fromJson(QJsonDocument::fromJson(QJsonDocument(j).toJson()).object());
        if (CHECK(back.has_value())) CHECK(*back == s);
        // The contract's example reads
        QString error;
        CHECK(!Direction::fromJson(QJsonObject{{"id", "d1"}}, &error) && !error.isEmpty()); // no recipe
        CHECK(!Direction::fromJson(QJsonObject{{"recipe", "short"}, {"suggestions", QJsonArray{QJsonObject{{"n", 1}}}}}));
        // Order: the given ones that are on, then the others on by n
        Direction::Session o = s;
        o.suggestions[1].on = true;
        CHECK(Direction::resolvedOrder(o) == (QVector<int>{3, 1, 2}));
        o.order = {9, 2};
        CHECK(Direction::resolvedOrder(o) == (QVector<int>{2, 1, 3}));
        // Script takes: another take switched on goes to its sentence's place, not to the end (videotest 21.11.)
        Direction::Session t;
        for (int n = 1; n <= 6; ++n) {
            Direction::Suggestion x;
            x.n = n;
            x.kind = "take";
            x.sentence = n <= 3 ? 1 : n - 2; // 1-3: sentence 1, 4: 2, 5: 3, 6: 4
            x.on = n != 2 && n != 3;
            t.suggestions << x;
        }
        t.order = {1, 4, 5, 6};
        t.suggestions[0].on = false;
        t.suggestions[1].on = true; // take 2 instead of 1
        CHECK(Direction::resolvedOrder(t) == (QVector<int>{2, 4, 5, 6}));
        t.suggestions[0].on = true; // both takes of sentence 1
        CHECK(Direction::resolvedOrder(t) == (QVector<int>{1, 2, 4, 5, 6}));
    }
    // ---- Every reason key has an English and a UI text; unknown keys show themselves
    {
        CHECK_EQ(Direction::reasonKeys().size(), 15);
        for (const QString& k : Direction::reasonKeys()) {
            const QString en = Direction::reasonText({k, {{"n", 3}, {"s", 1.5}, {"word", "äh"}, {"slot", "Intro"}, {"sentence", 2}}});
            CHECK(!en.isEmpty() && en != k && !en.contains('{'));
            const QString ui = Direction::reasonText({k, {{"n", 3}, {"s", 1.5}}}, Direction::Lang::Ui);
            CHECK(!ui.isEmpty() && ui != k);
        }
        CHECK_EQ(Direction::reasonText({"kills", {{"n", 1}}}), QString("1 kill"));
        CHECK_EQ(Direction::reasonText({"nonsense", {}}), QString("nonsense"));
        for (const QString& k : Direction::noteKeys()) CHECK(!Direction::noteText(k, {{"n", 2}}).isEmpty());
        CHECK(Direction::noteText("nonsense").isEmpty());
    }

    // ---- The project key: written only when set, read back, old files without it load
    QTemporaryDir dir;
    {
        ProjectData d;
        d.timeline = Timeline{};
        const QByteArray without = ProjectFile::toJson(d, dir.filePath("a.schneidi"));
        CHECK(!without.contains("\"direction\""));
        d.direction = Direction::toJson(example());
        const QByteArray with = ProjectFile::toJson(d, dir.filePath("a.schneidi"));
        CHECK(with.contains("\"direction\""));
        ProjectData back;
        QString error;
        CHECK(ProjectFile::fromJson(with, dir.filePath("a.schneidi"), &back, &error));
        CHECK(back.direction == d.direction);
        ProjectData old;
        CHECK(ProjectFile::fromJson(without, dir.filePath("a.schneidi"), &old, &error));
        CHECK(old.direction.isEmpty());
    }
    // ---- Set without an undo step, but the project is changed
    {
        Project p;
        p.reset();
        CHECK(!p.isModified());
        const int undo = p.undoStack()->count();
        p.setDirection(Direction::toJson(example()));
        CHECK(p.isModified());
        CHECK_EQ(p.undoStack()->count(), undo);
        CHECK(p.data().direction == p.direction());
    }
    // ---- A plan made before stays valid when the direction is set (not part of its base)
    {
        const QString path = dir.filePath("plan.schneidi");
        run("new", {{"project", path}});
        const QJsonObject planned = run("edit", {{"project", path}, {"plan", true},
                                                 {"ops", QJsonArray{QJsonObject{{"op", "add_track"}, {"kind", "video"}}}}});
        const QString id = planned["plan"]["id"].toString();
        CHECK(!id.isEmpty());
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(path, &d, &error));
        d.direction = Direction::toJson(example());
        CHECK(ProjectFile::save(d, path, &error));
        const QJsonObject ran = run("edit", {{"project", path}, {"plan_id", id}, {"run", "all"}});
        CHECK(!ran.contains("error"));
        // The hidden op of `direct` apply sets it (and is not listed in the help)
        run("edit", {{"project", path}, {"ops", QJsonArray{QJsonObject{{"op", "_direction"}, {"session", Direction::toJson(example())}}}}});
        ProjectData after;
        CHECK(ProjectFile::load(path, &after, &error));
        CHECK_EQ(after.direction["id"].toString(), QString("d3"));
        const QJsonObject help = run("help", {{"command", "edit"}});
        CHECK(!QJsonDocument(help).toJson().contains("_direction"));
    }
    return Check::result();
}
