// SPDX-License-Identifier: AGPL-3.0-only
// Proxy media without the Media Pool: Playback › Generate Proxy Media (selected clips / the timeline / the whole pool),
// the timeline clip menu, proxies on import (Settings › General: never - the default -, heavy video, always).
// ProxyManager::heavyVideo/wantedOnImport on synthetic files (4K, H.265, 10 bit, HD, a still, a sound file);
// core/ProxyChoice: which files each entry queues; the menu texts say why an entry is greyed out.
// PROXY_MENU_DUMP=<folder>: pictures of the menus and the settings group; PROXY_MENU_LANG=ru (another language),
// PROXY_MENU_FONT=1.25 (larger font) to check that the texts fit.

#include "check.h"

#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProxyChoice.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <clocale>

namespace {

QString dumpDir() { return qEnvironmentVariable("PROXY_MENU_DUMP"); }

void dump(QWidget* w, const QString& name)
{
    if (!dumpDir().isEmpty()) w->grab().save(dumpDir() + "/" + name + "-" + qEnvironmentVariable("PROXY_MENU_LANG", "de") + ".png");
}

void settle()
{
    QTest::qWait(30);
    QApplication::processEvents();
}

struct Files {
    QString hd, uhd, hevc, tenBit, still, sound, uhd2, hd2;
};

Files makeFiles(const QString& dir)
{
    const auto video = [&](const QString& name, const QString& size, const QStringList& codec) {
        return Check::makeMedia(dir + "/" + name, QStringList{"-f", "lavfi", "-i",
                                                              QString("testsrc2=size=%1:rate=25:duration=0.4").arg(size)}
                                                      + codec);
    };
    const QStringList h264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    Files f;
    f.hd = video("hd.mp4", "1280x720", h264);
    f.hd2 = video("hd2.mp4", "1280x720", h264);
    f.uhd = video("uhd.mp4", "3840x2160", h264);
    f.uhd2 = video("uhd2.mp4", "3840x2160", h264);
    // Optional encoders (the build machine may lack them): those checks are skipped then
    f.hevc = video("hevc.mp4", "640x360", {"-c:v", "libx265", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    f.tenBit = video("ten.mp4", "640x360", {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p10le"});
    f.still = Check::makeMedia(dir + "/still.png", {"-f", "lavfi", "-i", "color=red:size=64x64", "-frames:v", "1"});
    f.sound = Check::makeMedia(dir + "/tone.wav", {"-f", "lavfi", "-i", "sine=frequency=440:duration=0.5"});
    return f;
}

void testHeavy()
{
    CHECK(!ProxyManager::heavyVideo(1920, 1080, "h264", 8));
    CHECK(!ProxyManager::heavyVideo(2560, 1440, "h264", 8));
    CHECK(ProxyManager::heavyVideo(3840, 2160, "h264", 8));
    CHECK(ProxyManager::heavyVideo(2160, 3840, "h264", 8)); // portrait 4K (phone)
    CHECK(ProxyManager::heavyVideo(4096, 2160, "mpeg4", 8));
    CHECK(ProxyManager::heavyVideo(1920, 1080, "hevc", 8));
    CHECK(ProxyManager::heavyVideo(1280, 720, "libdav1d", 8));
    CHECK(ProxyManager::heavyVideo(1280, 720, "av1", 8));
    CHECK(ProxyManager::heavyVideo(1920, 1080, "h264", 10));
    CHECK(!ProxyManager::heavyVideo(0, 0, {}, 8));
}

void testRuleOnFiles(const Files& f)
{
    using R = ProxyManager::ImportRule;
    for (const QString& p : {f.hd, f.uhd, f.still, f.sound}) CHECK(!ProxyManager::wantedOnImport(R::Never, p));
    CHECK(!ProxyManager::wantedOnImport(R::Heavy, f.hd));
    CHECK(ProxyManager::wantedOnImport(R::Heavy, f.uhd));
    if (!f.hevc.isEmpty()) CHECK(ProxyManager::wantedOnImport(R::Heavy, f.hevc));
    if (!f.tenBit.isEmpty()) CHECK(ProxyManager::wantedOnImport(R::Heavy, f.tenBit));
    CHECK(!ProxyManager::wantedOnImport(R::Heavy, f.still));
    CHECK(!ProxyManager::wantedOnImport(R::Always, f.still));
    CHECK(!ProxyManager::wantedOnImport(R::Always, f.sound));
    CHECK(ProxyManager::wantedOnImport(R::Always, f.hd));
    CHECK(!ProxyManager::wantedOnImport(R::Always, f.hd + ".missing"));

    // The setting: never by default, stored
    QSettings().remove("proxy/onImport");
    CHECK(ProxyManager::importRule() == R::Never);
    ProxyManager::setImportRule(R::Heavy);
    CHECK(ProxyManager::importRule() == R::Heavy);
    ProxyManager::setImportRule(R::Always);
    CHECK(ProxyManager::importRule() == R::Always);
    ProxyManager::setImportRule(R::Never);
    CHECK(ProxyManager::importRule() == R::Never);
}

void testLabels()
{
    const QString a = "X";
    CHECK_EQ(ProxyChoice::label(a, 3, 2, 0), QString("X (2)"));
    CHECK(ProxyChoice::label(a, 0, 0, 0) != ProxyChoice::label(a, 2, 0, 0)); // no video vs. already there
    CHECK(ProxyChoice::label(a, 2, 0, 1) != ProxyChoice::label(a, 2, 0, 0)); // being made vs. already there
}

// Waits until `f` holds (at most `ms`)
template <class F> bool waitFor(F f, int ms = 8000)
{
    for (int t = 0; t < ms && !f(); t += 50) QTest::qWait(50);
    return f();
}

// The popup `open` shows, grabbed and closed again (QMenu::exec blocks)
QStringList popupTexts(const std::function<void()>& open, const QString& dumpName, QStringList* names = nullptr)
{
    QStringList texts;
    QTimer::singleShot(300, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* a : menu->actions()) {
                texts << a->text();
                if (names) *names << a->objectName();
            }
            dump(menu, dumpName);
            menu->close();
        }
    });
    open();
    settle();
    return texts;
}

void testApp(MainWindow& w, Engine& engine, const Files& f)
{
    Project* project = w.findChild<Project*>();
    Editor* editor = w.findChild<Editor*>();
    if (!CHECK(project && editor)) return;
    ProxyManager* proxies = engine.proxies();
    using S = ProxyChoice::Scope;

    // hd on the timeline, uhd/still/sound only in the pool
    w.importFiles({f.hd}, true);
    w.importFiles({f.uhd, f.still, f.sound});
    settle();
    CHECK(waitFor([&] { return project->mediaInfo(f.sound) != nullptr; }));
    CHECK_EQ(w.proxyFiles(int(S::Timeline)), QStringList{f.hd});
    const QStringList pool = w.proxyFiles(int(S::Pool));
    CHECK_EQ(pool.size(), 2); // no still, no sound file
    CHECK(pool.contains(f.hd) && pool.contains(f.uhd));
    // Selected: nothing chosen anywhere -> nothing; the hd clip chosen -> its file (video or audio part, once)
    editor->selection()->set({});
    CHECK(w.proxyFiles(int(S::Selected)).isEmpty());
    QSet<int> ids;
    for (const QVector<Track>* tracks : {&project->timeline().video, &project->timeline().audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips) ids.insert(c.id);
    CHECK(!ids.isEmpty());
    editor->selection()->set(ids);
    CHECK_EQ(w.proxyFiles(int(S::Selected)), QStringList{f.hd});

    // Generate for the timeline: hd queued, the pool's other file not
    CHECK_EQ(w.generateProxies(int(S::Timeline)), 1);
    CHECK(proxies->isPending(f.hd));
    CHECK(!proxies->isPending(f.uhd));
    // Again: nothing missing (it is being made)
    CHECK_EQ(w.generateProxies(int(S::Timeline)), 0);
    proxies->cancelAll();
    settle();
    CHECK(!proxies->isPending(f.hd));
    // A proxy there: not missing, "already there"
    QDir().mkpath(ProxyManager::cacheDir());
    {
        QFile fake(ProxyManager::proxyPath(f.hd));
        CHECK(fake.open(QIODevice::WriteOnly));
    }
    CHECK(proxies->hasProxy(f.hd));
    CHECK(proxies->missing({f.hd, f.uhd}) == QStringList{f.uhd});

    // Playback › Generate Proxy Media: the three entries with their counts
    QMenu* menu = w.findChild<QMenu*>("proxy_generate_menu");
    if (CHECK(menu)) {
        emit menu->aboutToShow();
        const QList<QAction*> a = menu->actions();
        if (CHECK_EQ(a.size(), 3)) {
            CHECK(!a[0]->isEnabled()); // selected: hd has its proxy
            CHECK(!a[1]->isEnabled()); // timeline: the same
            CHECK(a[2]->isEnabled());  // pool: uhd missing
            CHECK(a[2]->text().endsWith("(1)"));
            CHECK(a[0]->text() != T("für ausgewählte Clips")); // says why it is greyed out
        }
        for (QMenu* top : w.menuBar()->findChildren<QMenu*>())
            if (top->actions().contains(menu->menuAction())) {
                top->popup(w.mapToGlobal(QPoint(200, 30)));
                settle();
                menu->popup(top->geometry().topRight());
                settle();
                dump(top, "playback-menu");
                dump(menu, "proxy-submenu");
                menu->close();
                top->close();
            }
    }
    // The timeline clip menu: generate (greyed: there) and delete
    TimelineView* tv = w.findChild<TimelinePanel*>()->view();
    QStringList names;
    popupTexts([&] { emit tv->clipMenuRequested(w.mapToGlobal(QPoint(400, 500))); }, "clip-menu", &names);
    CHECK(names.contains("clip_proxy_generate"));
    CHECK(names.contains("clip_proxy_remove"));
    proxies->remove({f.hd});
    CHECK(!proxies->hasProxy(f.hd));
    names.clear();
    popupTexts([&] { emit tv->clipMenuRequested(w.mapToGlobal(QPoint(400, 500))); }, "clip-menu-missing", &names);
    CHECK(names.contains("clip_proxy_generate"));
    CHECK(!names.contains("clip_proxy_remove"));

    // On import: never (the default) queues nothing; heavy queues the 4K file only; a loaded project counts as no import
    w.importFiles({f.hd2});
    settle();
    QTest::qWait(500);
    CHECK(!proxies->isPending(f.hd2));
    ProxyManager::setImportRule(ProxyManager::ImportRule::Heavy);
    w.importFiles({f.uhd2});
    CHECK(waitFor([&] { return proxies->isPending(f.uhd2); }));
    CHECK(!proxies->isPending(f.hd2));
    proxies->cancelAll();
    ProxyManager::setImportRule(ProxyManager::ImportRule::Never);

    // Settings: the choice in the dialog, never first
    {
        SettingsDialog dlg(AiSettings{});
        auto* combo = dlg.findChild<QComboBox*>("proxy_import_rule");
        if (CHECK(combo)) {
            CHECK_EQ(combo->count(), 3);
            CHECK_EQ(combo->itemData(0).toInt(), int(ProxyManager::ImportRule::Never));
            CHECK(dlg.proxyImportRule() == ProxyManager::ImportRule::Never);
            combo->setCurrentIndex(1);
            CHECK(dlg.proxyImportRule() == ProxyManager::ImportRule::Heavy);
            dlg.resize(dlg.sizeHint());
            dlg.show();
            settle();
            if (auto* box = qobject_cast<QGroupBox*>(combo->parentWidget())) dump(box, "settings-proxies");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("proxy-menus");
    I18n::install(qEnvironmentVariable("PROXY_MENU_LANG", "de"));
    testHeavy();
    testLabels();
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir dir;
    const Files files = makeFiles(dir.path());
    if (!CHECK(!files.hd.isEmpty() && !files.uhd.isEmpty() && !files.still.isEmpty() && !files.sound.isEmpty()))
        return Check::result();
    testRuleOnFiles(files);
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    if (const double scale = qEnvironmentVariable("PROXY_MENU_FONT").toDouble(); scale > 0) {
        QFont font = QApplication::font();
        font.setPointSizeF(font.pointSizeF() * scale);
        QApplication::setFont(font);
    }
    w.resize(1600, 950);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    testApp(w, engine, files);
    return Check::result();
}
