// SPDX-License-Identifier: AGPL-3.0-only
// The Direct page with its real backend (cli/DirectBackendReal: the `direct` command, as main.cpp sets it): two
// synthetic clips dropped as material -> recipe short -> suggestions from the real analysis in the background ->
// Übernehmen = exactly one undo step with the cut and its 9:16 copy as new timelines; undo takes both back. The
// recipes that are not there yet (talk, template, script) show as coming and ask for nothing; a session the chat's
// `direct` made (LiveHost::liveDirection) shows on the page.

#include "check.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "cli/DirectBackendReal.h"
#include "cli/Live.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/direct/DirectMaterial.h"
#include "ui/direct/DirectPage.h"
#include "ui/direct/DirectRecipes.h"
#include "ui/direct/DirectSuggestions.h"

#include <QApplication>
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
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("direct-real");
    Theme::apply(app);
    std::setlocale(LC_NUMERIC, "C");
    I18n::install("de");
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

    // Talk, template and script are not there yet: their titles off, saying so
    for (const char* id : {"talk", "template", "script"}) {
        auto* t = page->findChild<QToolButton*>(QString("direct_recipe_") + id);
        if (CHECK(t)) {
            CHECK(!t->isEnabled());
            CHECK(t->text().contains("kommt gleich"));
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
    for (const Sequence& s : p->sequences()) tall |= s.name == "Short";
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
    return Check::result();
}
