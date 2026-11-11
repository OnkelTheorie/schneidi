// SPDX-License-Identifier: AGPL-3.0-only
// Proxy quality (Settings › General › Proxy media): the longer edge of a proxy per level (Small 540p, the default,
// Medium 720p, Large 1080p, Full = the original's size, never enlarged), stored for all projects; another level means
// other proxy files (the old ones stay, cleaning up calls them "other size"); a proxy being made when the level
// changes is made again at the new size; the main window offers to make the project's old proxies again; the viewer
// says "Proxy 540p" while its picture comes from proxy files and nothing while it plays originals.
#include "check.h"

#include "app/MainWindow.h"
#include "engine/Engine.h"
#include "engine/HardwareEncoders.h"
#include "engine/ProxyCleanup.h"
#include "engine/ProxyManager.h"
#include "engine/video/Reader.h"
#include "ui/Viewer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>

namespace {

using Q = ProxyManager::Quality;

QSize pictureSize(const QString& path)
{
    const std::unique_ptr<Video::Reader> r = Video::Reader::open(path, {});
    return r ? QSize(r->info().width, r->info().height) : QSize();
}

// Waits until `pm` has nothing pending for `original` (at most 2 minutes)
void waitDone(ProxyManager& pm, const QString& original)
{
    QElapsedTimer t;
    t.start();
    while (pm.isPending(original) && t.elapsed() < 120000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents();
}

void testLevels()
{
    CHECK(ProxyManager::quality() == Q::Small); // the default
    CHECK_EQ(ProxyManager::maxSize(), ProxyManager::kMaxSize);
    CHECK_EQ(ProxyManager::maxSizeOf(Q::Small), 960);
    CHECK_EQ(ProxyManager::maxSizeOf(Q::Medium), 1280);
    CHECK_EQ(ProxyManager::maxSizeOf(Q::Large), 1920);
    CHECK(ProxyManager::maxSizeOf(Q::Full) >= 16384);
    CHECK_EQ(ProxyManager::qualityLabel(Q::Small), QString("540p"));
    CHECK_EQ(ProxyManager::qualityLabel(Q::Large), QString("1080p"));

    // Each level its own file; the default path is the current level's
    const QString f = QDir::temp().filePath("schneidi-proxy-quality-name.mp4");
    QStringList names;
    for (Q q : {Q::Small, Q::Medium, Q::Large, Q::Full}) names << ProxyManager::proxyPath(f, ProxyManager::maxSizeOf(q));
    names.removeDuplicates();
    CHECK_EQ(names.size(), 4);
    CHECK_EQ(ProxyManager::proxyPath(f), ProxyManager::proxyPath(f, 960));

    // Encoder: x264 a bit better at full resolution, OpenH264 (GPL-free build) more bits for bigger proxies
    const QStringList small = HardwareEncoders::proxyVideoArguments(QSize(960, 540), false);
    const QStringList full = HardwareEncoders::proxyVideoArguments(QSize(3840, 2160), true);
    if (small.contains("libx264")) {
        CHECK_EQ(small.at(small.indexOf("-crf") + 1), QString("23"));
        CHECK_EQ(full.at(full.indexOf("-crf") + 1), QString("21"));
    } else {
        CHECK_EQ(small.at(small.indexOf("-b:v") + 1), QString("6M"));
        CHECK_EQ(full.at(full.indexOf("-b:v") + 1), QString("100M"));
        const QStringList hd = HardwareEncoders::proxyVideoArguments(QSize(1280, 720), false);
        CHECK_EQ(hd.at(hd.indexOf("-b:v") + 1), QString("11M"));
    }
}

// Real proxies of a 1440p clip at every level: the longer edge follows the level, never above the original
void testSizes(const QString& dir)
{
    const QString original = Check::makeMedia(dir + "/qhd.mp4", {"-f", "lavfi", "-i", "testsrc2=size=2560x1440:rate=25:duration=1",
                                                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!original.isEmpty())) return;
    ProxyManager pm;
    QString message;
    QObject::connect(&pm, &ProxyManager::failed, [&](const QString&, const QString& m) { message = m; });
    int qualitySignals = 0;
    QObject::connect(&pm, &ProxyManager::qualityChanged, [&] { ++qualitySignals; });
    const QList<std::pair<Q, QSize>> want{{Q::Small, {960, 540}}, {Q::Medium, {1280, 720}},
                                           {Q::Large, {1920, 1080}}, {Q::Full, {2560, 1440}}};
    for (const auto& [q, size] : want) {
        pm.setQuality(q);
        CHECK(ProxyManager::quality() == q);
        CHECK(!pm.hasProxy(original)); // the other levels' files do not count
        pm.generate({original});
        waitDone(pm, original);
        if (!message.isEmpty()) std::printf("proxy failed: %s\n", qPrintable(message));
        CHECK(pm.hasProxy(original));
        CHECK_EQ(pictureSize(ProxyManager::proxyPath(original)), size);
    }
    CHECK_EQ(qualitySignals, 3); // Small was already set: no signal for it
    CHECK_EQ(QSettings().value("proxy/quality").toString(), QString("full"));

    // All four files are there; withProxy finds the files of a level
    for (const auto& [q, size] : want)
        CHECK_EQ(ProxyManager::withProxy({original, original, dir + "/none.mp4"}, ProxyManager::maxSizeOf(q)),
                 QStringList{original});

    // Cleaning up: at level Large, the other levels' proxies are "other size", Large's is the project's (kept)
    pm.setQuality(Q::Large);
    const ProxyCleanup::Plan plan = ProxyCleanup::plan({original});
    int otherSize = 0;
    for (const ProxyCleanup::Item& it : plan.items) {
        if (it.file.endsWith(".json")) continue;
        CHECK(it.reason == ProxyCleanup::Reason::OtherSize);
        CHECK(it.file != QFileInfo(ProxyManager::proxyPath(original)).absoluteFilePath());
        ++otherSize;
    }
    CHECK_EQ(otherSize, 3);
    CHECK_EQ(plan.kept, 1);

    // Another process (the app) set Medium meanwhile; this one (a long-running schneidi-cli/MCP) still has Large
    // cached: cleaning up follows the stored level - it keeps the 720p proxy and calls the 1080p one "other size"
    {
        QSettings().setValue("proxy/quality", "medium");
        const ProxyCleanup::Plan p2 = ProxyCleanup::plan({original});
        CHECK_EQ(p2.kept, 1);
        for (const ProxyCleanup::Item& it : p2.items)
            CHECK(it.file != QFileInfo(ProxyManager::proxyPath(original, 1280)).absoluteFilePath());
        CHECK(ProxyManager::quality() == Q::Medium);
        pm.setQuality(Q::Large);
    }

    // Switching back while a proxy is being made for the new level: the one of the old level is there, nothing is made
    // again (it would overwrite a good proxy)
    {
        pm.setQuality(Q::Medium);
        pm.remove({original});
        pm.setQuality(Q::Small);
        const QDateTime before = QFileInfo(ProxyManager::proxyPath(original, 960)).lastModified();
        pm.setQuality(Q::Medium);
        pm.generate({original});
        CHECK(pm.isPending(original));
        pm.setQuality(Q::Small);
        CHECK(!pm.isPending(original));
        CHECK_EQ(QFileInfo(ProxyManager::proxyPath(original, 960)).lastModified(), before);
    }

    // Changing the level while a proxy is being made: it is made again at the new size (not the old size under the
    // new name)
    pm.remove({original});
    pm.setQuality(Q::Small);
    pm.remove({original});
    pm.generate({original});
    CHECK(pm.isPending(original));
    pm.setQuality(Q::Medium);
    CHECK(pm.isPending(original));
    waitDone(pm, original);
    CHECK(pm.hasProxy(original));
    CHECK_EQ(pictureSize(ProxyManager::proxyPath(original)), QSize(1280, 720));
    CHECK(!QFileInfo::exists(ProxyManager::proxyPath(original, 960)));
    pm.setQuality(Q::Small);
}

QString modeText(Viewer* viewer)
{
    QApplication::processEvents(); // showsProxyChanged is queued
    QApplication::processEvents();
    const auto* label = viewer->findChild<QLabel*>("PanelTitle");
    return label ? label->text() : QString();
}

// The main window: the viewer says when it plays a proxy; another level offers to make the old proxies again
void testMainWindow(const QString& dir)
{
    const QString a = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=2",
                                                        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!a.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({a}, true);
    ProxyManager* proxies = engine.proxies();
    proxies->remove({a});
    auto* viewer = w.findChild<Viewer*>();
    if (!CHECK(viewer)) return;

    engine.showSource(a);
    QApplication::processEvents();
    CHECK(!engine.showsProxy());
    CHECK(!modeText(viewer).contains("Proxy"));

    proxies->generate({a});
    waitDone(*proxies, a);
    CHECK(proxies->hasProxy(a));
    engine.showSource(a); // the source view resolves its file when it opens it
    CHECK(engine.showsProxy());
    CHECK(modeText(viewer).contains("Proxy 540p"));

    // "Use proxy media" off: the original plays, no note (the source view reopens its file by itself)
    proxies->setEnabled(false);
    CHECK(!engine.showsProxy());
    CHECK(!modeText(viewer).contains("Proxy"));
    proxies->setEnabled(true);

    // Level Medium: asked whether to make the one old proxy again; yes makes it at 720p
    QString asked;
    QTimer::singleShot(0, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        asked = box->text();
        box->button(QMessageBox::Yes)->click();
    });
    w.setProxyQuality(Q::Medium);
    CHECK(asked.contains("Eine Datei"));
    CHECK(asked.contains("720p"));
    CHECK(proxies->isPending(a));
    // Meanwhile the source view plays the original (it does not keep reading the 540p file under a "720p" note)
    CHECK(!engine.showsProxy());
    CHECK(!modeText(viewer).contains("Proxy"));
    waitDone(*proxies, a);
    CHECK_EQ(pictureSize(ProxyManager::proxyPath(a)), QSize(1280, 720));
    engine.showSource(a);
    CHECK(modeText(viewer).contains("Proxy 720p"));

    // Back to Small: the 540p proxy is still there - nothing to ask, the source view takes it at once
    asked.clear();
    QTimer::singleShot(0, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        asked = box->text();
        box->button(QMessageBox::No)->click();
    });
    w.setProxyQuality(Q::Small);
    QApplication::processEvents();
    CHECK(asked.isEmpty());
    CHECK(!proxies->isPending(a));
    CHECK(engine.showsProxy());
    CHECK(modeText(viewer).contains("Proxy 540p"));
    w.close();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("proxy_quality");
    QSettings().remove("proxy/quality");
    QDir(ProxyManager::cacheDir()).removeRecursively(); // test paths (QStandardPaths test mode)
    testLevels();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initEngine();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testSizes(tmp.path());
    testMainWindow(tmp.path());
    QSettings().remove("proxy/quality");
    QDir(ProxyManager::cacheDir()).removeRecursively();
    return Check::result();
}
