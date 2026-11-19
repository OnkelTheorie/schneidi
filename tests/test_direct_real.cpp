// SPDX-License-Identifier: AGPL-3.0-only
// The Direct page with its real backend (cli/DirectBackendReal: the `direct` command, as main.cpp sets it): two
// synthetic clips dropped as material -> recipe short -> suggestions from the real analysis in the background ->
// Übernehmen = exactly one undo step with the cut and its 9:16 copy as new timelines; undo takes both back. The
// recipes that are not there yet (talk, template, script) show as coming and ask for nothing; a session the chat's
// `direct` made (LiveHost::liveDirection) shows on the page.

#include "check.h"
#include "synth_sounds.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "cli/Commands.h"
#include "cli/DirectBackendReal.h"
#include "cli/Live.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/Extensions.h"
#include "ui/direct/DirectMaterial.h"
#include "ui/direct/DirectPage.h"
#include "ui/direct/DirectRecipes.h"
#include "ui/direct/DirectSuggestions.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <clocale>
#include <functional>

namespace {

// A fake whisper-cli (this program under that name): one word "Hallo" at 0.1-0.9 s
int fakeWhisper(int argc, char** argv)
{
    QString out;
    for (int i = 1; i + 1 < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == "-of") out = QString::fromLocal8Bit(argv[i + 1]);
    const QJsonObject json{{"result", QJsonObject{{"language", "de"}}},
                           {"transcription", QJsonArray{QJsonObject{{"text", " Hallo"},
                                                                    {"offsets", QJsonObject{{"from", 100}, {"to", 900}}}}}}};
    QFile o(out + ".json");
    if (!o.open(QIODevice::WriteOnly)) return 4;
    o.write(QJsonDocument(json).toJson());
    return 0;
}

bool waitFor(const std::function<bool()>& ok, int ms = 120000)
{
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < ms) QTest::qWait(30);
    return ok();
}

} // namespace

int main(int argc, char** argv)
{
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).baseName() == QLatin1String("whisper-cli")) return fakeWhisper(argc, argv);
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("direct-real");
    Theme::apply(app);
    std::setlocale(LC_NUMERIC, "C");
    I18n::install(qEnvironmentVariable("DIRECT_REAL_LANG", "de"));
    if (const double scale = qEnvironmentVariable("DIRECT_REAL_FONT", "1").toDouble(); scale > 1) {
        QFont f = QApplication::font();
        f.setPointSizeF(f.pointSizeF() * scale);
        QApplication::setFont(f);
    }
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir dir;
    // Two "recordings", each with a loud moment
    const auto clip = [&](const QString& name, int at) {
        return Check::makeMedia(dir.path() + "/" + name,
                                {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=20", "-f", "lavfi", "-i",
                                 QString("aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,%1,%2),0.8*sin(2*PI*440*t),0)':d=20:s=48000")
                                     .arg(at)
                                     .arg(at + 0.6),
                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    };
    const QString a = clip("a.mp4", 6), b = clip("b.mp4", 12);
    if (!CHECK(!a.isEmpty() && !b.isEmpty())) return Check::result();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();

    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
    w.setDirectBackend(std::make_unique<RealDirectBackend>(&w));
    w.show();
    if (!CHECK(QTest::qWaitForWindowExposed(&w))) return Check::result();
    {
        ProjectData d;
        d.format.rate = {25, 1};
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        w.liveProject()->load(d);
        w.liveProject()->undoStack()->clear();
    }
    Project* p = w.liveProject();
    CHECK(w.liveShowPage("direct"));
    DirectPage* page = w.directPage();
    if (!CHECK(page)) return Check::result();

    // Every recipe is there with the real backend
    for (const char* id : {"short", "highlights", "talk", "template", "script"}) {
        auto* t = page->findChild<QToolButton*>(QString("direct_recipe_") + id);
        if (CHECK(t)) {
            CHECK(t->isEnabled());
            CHECK(!t->text().contains("kommt gleich"));
        }
    }
    auto* shortChip = page->findChild<QToolButton*>("direct_recipe_short");
    CHECK(shortChip && shortChip->isEnabled());

    // Real suggestions in the background; the project stays unmodified
    page->material()->addFiles({a, b});
    page->recipes()->setRecipe("short");
    page->recipes()->suggestButton()->click();
    CHECK(waitFor([&] { return page->suggestions()->hasSession() && !page->busy(); }));
    const QJsonObject session = page->suggestions()->session();
    CHECK(session.value("id").toString().startsWith("d"));
    CHECK(!session.value("suggestions").toArray().isEmpty());
    CHECK(!page->suggestions()->checkedOrder().isEmpty());
    CHECK(!p->isModified());

    // Übernehmen: one undo step, the cut and the 9:16 copy as new timelines
    const int undo = p->undoStack()->count();
    const int timelines = int(p->sequences().size());
    CHECK(page->applyButton()->isEnabled());
    page->applyButton()->click();
    CHECK(page->applying()); // "Baut die Timeline …" shows before the work starts
    CHECK(waitFor([&] { return !page->applying(); }));
    CHECK_EQ(p->undoStack()->count(), undo + 1);
    CHECK_EQ(int(p->sequences().size()), timelines + 2);
    bool tall = false;
    for (const Sequence& s : p->sequences()) tall |= s.name == T("Short");
    CHECK(tall);
    CHECK_EQ(p->direction().value("id").toString(), session.value("id").toString());
    // No whisper here and no effect sounds chosen: nothing to finish, the second button stays away
    CHECK(!page->finishButton()->isVisible());
    p->undoStack()->undo();
    CHECK_EQ(int(p->sequences().size()), timelines);

    // With effect sounds chosen the finish is offered and is one more undo step
    {
        QJsonObject withSfx = session;
        QJsonObject o = withSfx.value("options").toObject();
        o["sfx"] = "mild";
        withSfx["options"] = o;
        w.liveDirection(withSfx, true); // as the chat's `direct` would show it
        CHECK_EQ(page->suggestions()->session().value("id").toString(), session.value("id").toString());
        page->applyButton()->click();
        CHECK(waitFor([&] { return !page->applying(); }));
        if (CHECK(page->finishButton()->isVisible())) {
            const int before = p->undoStack()->count();
            page->finishButton()->click();
            CHECK(waitFor([&] { return !page->applying(); }));
            CHECK_EQ(p->undoStack()->count(), before + 1);
            CHECK(!page->finishButton()->isVisible());
        }
    }
    // Recipe talk on the current timeline: two microphones, their common pause found, Übernehmen = one undo step
    {
        const int rate = 48000;
        Synth::Builder ma(rate, 11.0, 11), mb(rate, 11.0, 12);
        ma.floor();
        Synth::speech(ma, 0.2, 2.7);
        Synth::speech(ma, 5.0, 1.6);
        Synth::speech(ma, 9.0, 1.0, 120);
        mb.floor();
        Synth::speech(mb, 2.9, 2.5, 180);
        const QString wa = dir.path() + "/mic-a.wav", wb = dir.path() + "/mic-b.wav", path = dir.path() + "/talk.schneidi";
        Synth::writeWav(wa, ma.s, rate);
        Synth::writeWav(wb, mb.s, rate);
        const auto cli = [](const QString& name, const QJsonObject& raw) {
            const Cli::Command* cmd = Cli::find(name);
            Cli::Context ctx;
            return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
        };
        cli("new", {{"project", path}, {"width", 320}, {"height", 180}, {"fps", 25}});
        cli("edit", {{"project", path},
                     {"ops", QJsonArray{QJsonObject{{"op", "place"}, {"media", wa}, {"at", 0}, {"track", "A1"}},
                                        QJsonObject{{"op", "place"}, {"media", wb}, {"at", 0}, {"track", "A2"}}}}});
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(path, &d, &error));
        p->load(d);
        p->undoStack()->clear();
        page->material()->setUseTimeline(true);
        page->recipes()->setRecipe("talk");
        page->recipes()->suggestButton()->click();
        CHECK(waitFor([&] { return page->suggestions()->hasSession() && !page->busy()
                                   && page->suggestions()->session().value("recipe").toString() == "talk"; }));
        const QJsonArray pauses = page->suggestions()->session().value("suggestions").toArray();
        CHECK(!pauses.isEmpty());
        const int steps = p->undoStack()->count(), seqs = int(p->sequences().size());
        page->applyButton()->click();
        CHECK(waitFor([&] { return !page->applying(); }));
        CHECK_EQ(p->undoStack()->count(), steps + 1);
        CHECK_EQ(int(p->sequences().size()), seqs + 1);
    }
    // Recipe template with files as material: the shots for short-hook's fields, a new project to open
    {
        page->material()->setUseTimeline(false);
        page->material()->clear();
        page->material()->addFiles({a, b});
        page->recipes()->setRecipe("template");
        page->recipes()->setTemplate("short-hook", "Short Hook");
        page->recipes()->suggestButton()->click();
        CHECK(waitFor([&] { return !page->busy() && page->suggestions()->session().value("recipe").toString() == "template"; }));
        CHECK(!page->suggestions()->session().value("suggestions").toArray().isEmpty());
        const int steps = p->undoStack()->count();
        page->applyButton()->click();
        CHECK(waitFor([&] { return !page->applying(); }));
        CHECK_EQ(p->undoStack()->count(), steps); // nothing changed here: a project of its own
        CHECK(page->openProjectButton()->isVisible());
    }
    // Recipe script with a (fake) whisper on files of the material
    {
        const QString suffix = QFileInfo(QCoreApplication::applicationFilePath()).suffix();
        const QString fake = dir.filePath(suffix.isEmpty() ? "bin/whisper-cli" : "bin/whisper-cli." + suffix);
        QDir().mkpath(dir.filePath("bin"));
        QFile::copy(QCoreApplication::applicationFilePath(), fake);
        QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner);
        qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH")).toUtf8());
        const QString models = QDir(Extensions::rootDir()).filePath("whisper/models");
        QDir().mkpath(models);
        QFile m(QDir(models).filePath("ggml-small-q5_1.bin"));
        if (m.open(QIODevice::WriteOnly)) m.write("model");
        m.close();
        page->recipes()->setRecipe("script");
        if (auto* text = page->recipes()->card("script")->findChild<QPlainTextEdit*>(); CHECK(text)) text->setPlainText("Hallo.");
        page->recipes()->suggestButton()->click();
        CHECK(waitFor([&] { return !page->busy() && page->suggestions()->session().value("recipe").toString() == "script"; }));
        const QJsonArray takes = page->suggestions()->session().value("suggestions").toArray();
        if (CHECK(!takes.isEmpty())) CHECK_EQ(takes[0].toObject().value("kind").toString(), QString("take"));
        const int steps = p->undoStack()->count();
        page->applyButton()->click();
        CHECK(waitFor([&] { return !page->applying(); }));
        CHECK_EQ(p->undoStack()->count(), steps + 1);
    }
    // DIRECT_REAL_DUMP=<file.png>: the window with the page after its runs (layout check by eye)
    if (const QString dump = qEnvironmentVariable("DIRECT_REAL_DUMP"); !dump.isEmpty()) {
        page->setFinishOffered(true);
        page->setMadeProject(dir.filePath("Short Hook.schneidi"));
        QTest::qWait(200);
        w.grab().save(dump);
    }
    return Check::result();
}
