// SPDX-License-Identifier: AGPL-3.0-only
// Custom code on the Comp page (0.19, core/CodeTrust.h): an fx node whose filter chain / shader nobody trusted here
// says it is skipped (inspector and node box) and offers "Code erlauben" like the clip inspector; code typed into
// the node inspector is the user's own and runs. Applying a recipe with untrusted code (one from a ZIP) asks with the
// TrustDialog right after applying: "Ausführen" trusts and remembers it for the recipe file, "Nicht ausführen" keeps
// it skipped; the apply stays one undo step either way.

#include "check.h"

#include "app/MainWindow.h"
#include "core/CodeTrust.h"
#include "core/CompRecipes.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/picture/CustomEffects.h"
#include "ui/TrustDialog.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompPage.h"
#include "ui/comp/CompShared.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <clocale>
#include <functional>

namespace {

void settle()
{
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

QString g_asked; // the text of the last TrustDialog answered

// Clicks a button of the TrustDialog the next modal loop shows; *asked = it came
void answer(const char* button, bool* asked)
{
    *asked = false;
    QTimer::singleShot(0, [button, asked] {
        auto* dlg = qobject_cast<TrustDialog*>(QApplication::activeModalWidget());
        if (!dlg)
            for (QWidget* w : QApplication::topLevelWidgets())
                if (auto* t = qobject_cast<TrustDialog*>(w); t && t->isVisible()) dlg = t;
        if (!dlg) return;
        *asked = true;
        if (auto* text = dlg->findChild<QLabel*>("trustText")) g_asked = text->text();
        dlg->findChild<QPushButton*>(button)->click();
    });
}

// The picture the viewer shows next (the engine's frameReady); null when none comes within `ms`
QImage nextFrame(Engine& engine, const std::function<void()>& action, int ms = 3000)
{
    QImage got;
    const auto c = QObject::connect(&engine, &Engine::frameReady, [&got](const QImage& i) { got = i.copy(); });
    action();
    QElapsedTimer t;
    t.start();
    while (got.isNull() && !t.hasExpired(ms)) QApplication::processEvents(QEventLoop::AllEvents, 10);
    QObject::disconnect(c);
    return got;
}

// `b` is the negative of `a` where `a` has strong colors (at least one such place)
bool negativeOf(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return false;
    int strong = 0;
    for (int y = 1; y < 8; ++y)
        for (int x = 1; x < 8; ++x) {
            const QColor p = a.pixelColor(a.width() * x / 8, a.height() * y / 8);
            const QColor q = b.pixelColor(b.width() * x / 8, b.height() * y / 8);
            if (std::abs(p.red() - 128) < 60 && std::abs(p.green() - 128) < 60 && std::abs(p.blue() - 128) < 60) continue;
            ++strong;
            if (std::abs(255 - p.red() - q.red()) > 40 || std::abs(255 - p.green() - q.green()) > 40
                || std::abs(255 - p.blue() - q.blue()) > 40)
                return false;
        }
    return strong > 0;
}

void resetAll()
{
    CodeTrust::resetForTests();
    QSettings().remove("trust");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("comp-trust");
    I18n::setLanguage("de");
    I18n::install("de");
    if (!Check::haveFfmpeg()) {
        std::puts("SKIP: no ffmpeg for the test clip");
        return 77;
    }
    QTemporaryDir dir;
    resetAll();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    const QString file = Check::makeMedia(dir.path() + "/clip.mp4",
        {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p"});
    if (!CHECK(!file.isEmpty())) return Check::result();
    w.importFiles({file}, true);
    settle();
    Project* project = w.liveProject();
    QUndoStack* undo = project->undoStack();
    if (!CHECK(!project->timeline().video.isEmpty() && !project->timeline().video[0].clips.isEmpty()))
        return Check::result();
    const int clipId = project->timeline().video[0].clips[0].id;
    const auto clip = [&] { return TimelineOps::findClip(project->timeline(), clipId); };

    // ---- An fx node with untrusted code: skipped in inspector and node box, "Code erlauben" trusts it ----
    for (const auto& [effect, key, code] : {std::tuple<QString, QString, QString>{"ffmpeg_filter", "e_chain", "negate"},
                                            {"shader", "e_code", "fragColor = vec4(1.0) - texture(src, uv);"}}) {
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "fx", {}};
        g.links << CompLink{1, 2, "src"};
        g.out = 2;
        QHash<int, QVariantMap> values;
        values[2] = {{"effect", effect}, {key, code}};
        QString why;
        bool set = false;
        const QImage before = nextFrame(engine, [&] { set = w.editor()->setCompGraph({clipId}, g, values, CompUi::kinds(), &why); });
        if (!CHECK(set)) continue;
        CHECK(!CodeTrust::isTrusted(code)); // set by an op of someone else's file: not trusted
        w.openComp(clipId);
        CompPage* page = w.compPage();
        if (!CHECK(page)) return Check::result();
        page->selectNode(2);
        settle();
        CompInspector* ins = page->inspector();
        auto* allow = ins->findChild<QPushButton*>("allowCode");
        auto* skipped = ins->findChild<QLabel*>("comp_untrusted");
        if (!CHECK(allow && skipped)) continue;
        CHECK(allow->isVisibleTo(ins));
        CHECK(skipped->isVisibleTo(ins));
        CHECK_EQ(CompUi::untrustedCode(*clip(), 2), code);
        CHECK(CompUi::nodeSummary(*clip(), g.nodes[1]).endsWith("nicht ausgeführt"));
        const int idx = undo->index();
        // The paused picture shows the code at once, without a seek (trust is part of what the engine draws)
        const QImage after = nextFrame(engine, [&] { allow->click(); });
        if (effect == QLatin1String("ffmpeg_filter") && Picture::Custom::filterAvailable())
            CHECK(negativeOf(before, after));
        settle();
        CHECK(CodeTrust::isTrusted(code));
        CHECK_EQ(undo->index(), idx); // trust is no project change
        CHECK(!allow->isVisibleTo(ins));
        CHECK(!skipped->isVisibleTo(ins));
        CHECK(CompUi::untrustedCode(*clip(), 2).isEmpty());
        CHECK(!CompUi::nodeSummary(*clip(), g.nodes[1]).contains("nicht ausgeführt"));
    }

    // ---- Code typed into the node inspector is the user's own: it runs ----
    if (Picture::Custom::filterAvailable() && Picture::Custom::checkFilter("hflip").isEmpty()) {
        CompPage* page = w.compPage();
        page->selectNode(2);
        settle();
        CompInspector* ins = page->inspector();
        // The node is the shader one now; switch it back to a filter chain
        QString why;
        CHECK(w.editor()->setCompNodeValuesAt(clipId, 2, {{"effect", "ffmpeg_filter"}}, 0, {}, &why));
        settle();
        auto* edit = ins->findChild<QPlainTextEdit*>("comp_param_e_chain");
        auto* applyButton = ins->findChild<QPushButton*>("comp_apply_e_chain");
        if (CHECK(edit && applyButton)) {
            edit->setPlainText("hflip");
            applyButton->click();
            settle();
            CHECK(CodeTrust::isTrusted("hflip"));
            auto* allow = ins->findChild<QPushButton*>("allowCode");
            CHECK(allow && !allow->isVisibleTo(ins));
        }
    } else {
        std::puts("note: no FFmpeg filters here, typed code not checked");
    }

    // ---- A recipe with untrusted code (as from a ZIP): asked right after applying ----
    resetAll();
    // An own recipe nobody trusted here, as importZip leaves it (and once as a path to its file)
    const QString recipeDir = CompRecipes::dir() + "/trust_flip";
    QDir(recipeDir).removeRecursively();
    CHECK(QDir().mkpath(recipeDir));
    const QString recipe = recipeDir + "/recipe.json";
    {
        QFile f(recipe);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(R"({"name": "Flip", "description": "flips",
            "graph": {"nodes": [{"id": 1, "type": "clip"},
                                {"id": 2, "type": "fx", "params": {"effect": "ffmpeg_filter", "e_chain": "vflip"}}],
                      "links": [[1, 2, "src"]], "out": 2}})");
    }
    const std::optional<CompRecipe> r = CompRecipes::find("trust_flip", &err);
    if (!CHECK(r)) {
        std::printf("recipe: %s\n", qPrintable(err));
        return Check::result();
    }
    const QStringList codes = CompRecipes::customCode(*r);
    CHECK_EQ(codes, QStringList{"vflip"});
    {
        // "Nicht ausführen": applied (one undo step), the code stays skipped and is not remembered
        bool asked = false;
        const int idx = undo->index();
        answer("trustSkip", &asked);
        w.applyRecipe({clipId}, "trust_flip");
        settle();
        CHECK(asked);
        CHECK(g_asked.startsWith("Dieses Rezept enthält Code")); // a recipe, not a project
        CHECK_EQ(undo->index(), idx + 1);
        CHECK(!CodeTrust::isTrusted("vflip"));
        CHECK(!CodeTrust::remembered(r->file, codes));
        CHECK_EQ(CompUi::untrustedCode(*clip(), 2), QString("vflip"));
        undo->undo();
        CHECK_EQ(undo->index(), idx);

        // "Ausführen": trusted and remembered for the recipe file, still one undo step
        answer("trustRun", &asked);
        w.applyRecipe({clipId}, "trust_flip");
        settle();
        CHECK(asked);
        CHECK_EQ(undo->index(), idx + 1);
        CHECK(CodeTrust::isTrusted("vflip"));
        CHECK(CodeTrust::remembered(r->file, codes));
        CHECK(CompUi::untrustedCode(*clip(), 2).isEmpty());
        undo->undo();

        // The next run: remembered for the file, no question
        CodeTrust::resetForTests();
        answer("trustSkip", &asked);
        w.applyRecipe({clipId}, recipe);
        settle();
        CHECK(!asked);
        CHECK(CodeTrust::isTrusted("vflip"));
        undo->undo();

        // A recipe without custom code asks nothing
        answer("trustSkip", &asked);
        w.applyRecipe({clipId}, "greenscreen");
        settle();
        CHECK(!asked);
    }
    QDir(recipeDir).removeRecursively();
    resetAll();
    return Check::result();
}
