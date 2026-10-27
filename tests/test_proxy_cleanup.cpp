// Cleaning up the proxy folder (engine/ProxyCleanup): which proxy files go and why – the original is gone, changed,
// used by no open or recent project, made at another size, a half-written leftover, a proxy without a note (older
// version) – and which stay: the open project's proxies (never deleted), those of recent projects, the file being
// written, and anything in the folder that is not a proxy. A dry run (plan) deletes nothing; apply deletes exactly
// the plan and reports the bytes. The proxy's note names its original; deleting a proxy deletes its note too.
// The dialog: Cancel deletes nothing, confirming deletes the plan, nothing to do is said. Needs no ffmpeg: a proxy's
// name only depends on path, size and modification time of its original.
#include "check.h"

#include "app/ProxyCleanupDialog.h"
#include "core/ProjectFile.h"
#include "engine/ProxyCleanup.h"
#include "engine/ProxyManager.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

namespace {

void writeFile(const QString& path, qint64 bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QByteArray(int(bytes), 'x'));
}

// A finished proxy of `original` (bytes big) with its note, as ProxyManager leaves it
QString makeProxy(const QString& original, qint64 bytes, int maxSize = ProxyManager::kMaxSize)
{
    const QString proxy = ProxyManager::proxyPath(original);
    writeFile(proxy, bytes);
    ProxyCleanup::writeNote(proxy, original, maxSize);
    return proxy;
}

const ProxyCleanup::Item* find(const ProxyCleanup::Plan& p, const QString& file)
{
    for (const ProxyCleanup::Item& it : p.items)
        if (it.file == QFileInfo(file).absoluteFilePath()) return &it;
    return nullptr;
}

Clip clip(int id, const QString& path)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.out = 24;
    return c;
}

// The dialog (Playback › Clean Up Proxy Media…, Media Pool): says what goes and how much, lists why; Cancel deletes
// nothing, the button with the size deletes exactly that. PROXY_CLEANUP_DUMP=<png>: picture of it, details open.
void testDialog(const QString& folder)
{
    QDir(ProxyManager::cacheDir()).removeRecursively();
    QDir().mkpath(ProxyManager::cacheDir());
    QSettings().setValue("recentProjects", QStringList{});
    auto original = [&](const QString& name) {
        const QString p = folder + "/" + name;
        writeFile(p, 100);
        return p;
    };
    const QString kept = original("dialog-open.mp4");
    const QString a = original("dialog-a.mp4"), b = original("dialog-b.mp4");
    const QString keptProxy = makeProxy(kept, 2000);
    const QString pa = makeProxy(a, 3 * 1024 * 1024);
    const QString pb = makeProxy(b, 1024 * 1024);
    QFile::remove(b); // its original is gone
    qint64 expected = 0;
    for (const QString& f : {pa, pb}) expected += QFileInfo(f).size() + QFileInfo(ProxyCleanup::notePath(f)).size();
    ProjectData open;
    open.media << MediaInfo{kept, "dialog-open.mp4", 25, true, true, false};
    ProxyManager pm;

    QString text, details;
    bool shown = false;
    auto answer = [&](QMessageBox::ButtonRole role) {
        shown = false;
        QTimer::singleShot(300, [&, role] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box) return;
            shown = true;
            text = box->text();
            details = box->detailedText();
            for (QAbstractButton* button : box->buttons())
                if (box->buttonRole(button) == QMessageBox::ActionRole) button->click(); // "Show Details…"
            if (const QByteArray dump = qgetenv("PROXY_CLEANUP_DUMP"); !dump.isEmpty() && !details.isEmpty()) {
                QApplication::processEvents();
                box->grab().save(QString::fromLocal8Bit(dump));
            }
            for (QAbstractButton* button : box->buttons())
                if (box->buttonRole(button) == role) return button->click();
        });
    };
    answer(QMessageBox::RejectRole);
    CHECK_EQ(ProxyCleanupDialog::run(nullptr, open, &pm), 0);
    CHECK(shown);
    std::printf("       %s\n%s\n", qPrintable(text), qPrintable(details));
    CHECK(text.startsWith("2 von 3"));
    CHECK(text.contains(ProxyCleanup::sizeText(expected)));
    CHECK(details.contains("dialog-a.mp4") && details.contains("dialog-b.mp4"));
    CHECK(details.contains(ProxyCleanup::reasonText(ProxyCleanup::Reason::Missing)));
    CHECK(!details.contains("dialog-open.mp4"));
    CHECK(QFileInfo::exists(pa) && QFileInfo::exists(pb)); // canceled: nothing deleted

    answer(QMessageBox::DestructiveRole);
    CHECK_EQ(ProxyCleanupDialog::run(nullptr, open, &pm), expected);
    CHECK(!QFileInfo::exists(pa) && !QFileInfo::exists(pb));
    CHECK(QFileInfo::exists(keptProxy));

    // Nothing left to do: says so
    answer(QMessageBox::AcceptRole);
    CHECK_EQ(ProxyCleanupDialog::run(nullptr, open, &pm), 0);
    CHECK(shown);
    CHECK(text.startsWith("Nichts aufzuräumen"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("proxy_cleanup");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    QDir(ProxyManager::cacheDir()).removeRecursively(); // test paths (QStandardPaths test mode)
    QDir().mkpath(ProxyManager::cacheDir());
    const QString dir = ProxyManager::cacheDir();

    auto original = [&](const char* name, qint64 bytes = 1000) {
        const QString p = tmp.filePath(name);
        writeFile(p, bytes);
        return p;
    };
    const QString open = original("open.mp4");       // used by the open project
    const QString recent = original("recent.mp4");   // used by a recently opened project
    const QString unused = original("unused.mp4");   // there, but no project uses it
    const QString gone = original("gone.mp4");       // deleted after its proxy was made
    const QString changed = original("changed.mp4"); // changed after its proxy was made
    const QString small = original("small.mp4");     // proxy made at another size
    const QString busy = original("busy.mp4");       // its proxy is being written right now

    const QString openProxy = makeProxy(open, 5000);
    const QString recentProxy = makeProxy(recent, 4000);
    const QString unusedProxy = makeProxy(unused, 3000);
    const QString goneProxy = makeProxy(gone, 2500);
    QFile::remove(gone);
    const QString changedProxy = makeProxy(changed, 2000);
    writeFile(changed, 1500); // other size: maps to another proxy now
    const QString otherSizeProxy = makeProxy(small, 1800, ProxyManager::kMaxSize * 2);
    // Older version: no note, its original is unknown
    const QString noNote = ProxyManager::proxyPath(tmp.filePath("whatever.mp4"));
    writeFile(noNote, 1200);
    // Half-written files: one from a crash, one being written now, one of the open project's file (may be on its way)
    const QString crashPart = ProxyManager::proxyPath(unused).chopped(4) + ".part.mp4";
    writeFile(crashPart, 700);
    const QString busyPart = ProxyManager::proxyPath(busy).chopped(4) + ".part.mp4";
    writeFile(busyPart, 600);
    const QString openPart = ProxyManager::proxyPath(open).chopped(4) + ".part.mp4";
    writeFile(openPart, 500);
    // A note whose proxy is gone
    const QString orphanNote = dir + "/" + QString(40, 'a') + ".json";
    writeFile(orphanNote, 50);
    // Not ours: never touched
    const QString foreign1 = dir + "/readme.txt", foreign2 = dir + "/my-own.mp4";
    writeFile(foreign1, 10);
    writeFile(foreign2, 10);

    // ---- Dry run
    const QStringList keep{open, recent};
    const ProxyCleanup::Plan plan = ProxyCleanup::plan(keep, {busyPart});
    std::printf("       plan: %d files, %lld bytes, kept %d (%lld bytes)\n", int(plan.items.size()), plan.bytes, plan.kept,
                plan.keptBytes);
    auto reasonOf = [&](const QString& f) { const auto* it = find(plan, f); return it ? int(it->reason) : -1; };
    CHECK_EQ(reasonOf(unusedProxy), int(ProxyCleanup::Reason::Unused));
    CHECK_EQ(reasonOf(goneProxy), int(ProxyCleanup::Reason::Missing));
    CHECK_EQ(reasonOf(changedProxy), int(ProxyCleanup::Reason::Changed));
    CHECK_EQ(reasonOf(otherSizeProxy), int(ProxyCleanup::Reason::OtherSize));
    CHECK_EQ(reasonOf(noNote), int(ProxyCleanup::Reason::Unknown));
    CHECK_EQ(reasonOf(crashPart), int(ProxyCleanup::Reason::Leftover));
    CHECK_EQ(reasonOf(orphanNote), int(ProxyCleanup::Reason::Leftover));
    // Never: the open project's proxy (and its part), the recent project's, the one being written, foreign files
    for (const QString& f : {openProxy, openPart, recentProxy, busyPart, foreign1, foreign2,
                             ProxyCleanup::notePath(openProxy), ProxyCleanup::notePath(unusedProxy)})
        CHECK(!find(plan, f));
    CHECK_EQ(plan.items.size(), 7);
    CHECK_EQ(plan.kept, 2);
    if (const auto* it = find(plan, goneProxy)) {
        CHECK_EQ(it->original, QFileInfo(gone).absoluteFilePath());
        CHECK_EQ(it->bytes, 2500 + QFileInfo(ProxyCleanup::notePath(goneProxy)).size()); // with its note
    }
    qint64 sum = 0;
    for (const ProxyCleanup::Item& it : plan.items) sum += it.bytes;
    CHECK_EQ(plan.bytes, sum);
    CHECK(plan.items.first().bytes >= plan.items.last().bytes); // largest first
    CHECK(QFileInfo::exists(unusedProxy) && QFileInfo::exists(crashPart)); // a plan deletes nothing
    // Without the open project in `keep` its proxy would go: the caller always passes it (here: recent only)
    CHECK(find(ProxyCleanup::plan({recent}, {busyPart}), openProxy));

    // ---- Apply: exactly the plan
    const qint64 freed = ProxyCleanup::apply(plan, {busyPart});
    CHECK_EQ(freed, plan.bytes);
    for (const ProxyCleanup::Item& it : plan.items) CHECK(!QFileInfo::exists(it.file));
    CHECK(!QFileInfo::exists(ProxyCleanup::notePath(unusedProxy)));
    for (const QString& f : {openProxy, openPart, recentProxy, busyPart, foreign1, foreign2})
        CHECK(QFileInfo::exists(f));
    CHECK(QFileInfo::exists(ProxyCleanup::notePath(openProxy)));
    CHECK(QFileInfo::exists(open) && QFileInfo::exists(unused) && QFileInfo::exists(changed)); // originals stay
    const ProxyCleanup::Plan again = ProxyCleanup::plan(keep, {busyPart});
    CHECK(again.items.isEmpty());
    CHECK_EQ(again.kept, 2);
    // Leftover written meanwhile (busy by now): skipped by apply
    {
        const QString part = ProxyManager::proxyPath(small).chopped(4) + ".part.mp4";
        writeFile(part, 100);
        const ProxyCleanup::Plan p = ProxyCleanup::plan(keep);
        CHECK(find(p, part));
        CHECK_EQ(ProxyCleanup::apply(p, {part}), 0);
        CHECK(QFileInfo::exists(part));
        QFile::remove(part);
    }

    // ---- What a project uses: media pool and clips of every timeline, titles have no file
    ProjectData d;
    d.media << MediaInfo{open, "open.mp4", 25, true, true, false};
    d.timeline.video.resize(1);
    d.timeline.video[0].clips << clip(1, recent);
    Clip title;
    title.id = 2;
    title.kind = ClipKind::Title;
    d.timeline.video[0].clips << title;
    Sequence other;
    other.id = 7;
    other.timeline.audio.resize(1);
    other.timeline.audio[0].clips << clip(3, unused);
    d.sequences << other;
    const QStringList used = ProxyCleanup::projectMedia(d);
    CHECK_EQ(used.size(), 3);
    CHECK(used.contains(open) && used.contains(recent) && used.contains(unused));

    // ---- Recent projects (File › Recent): their media, read from disk; missing project files are skipped
    const QString projectFile = tmp.filePath("film.schneidi");
    QString error;
    CHECK(ProjectFile::save(d, projectFile, &error));
    QSettings().setValue("recentProjects", QStringList{projectFile, tmp.filePath("deleted.schneidi")});
    CHECK_EQ(ProxyCleanup::recentProjects().size(), 2);
    const QStringList recentMedia = ProxyCleanup::mediaOfProjects(ProxyCleanup::recentProjects());
    CHECK(recentMedia.contains(unused) && recentMedia.contains(recent) && recentMedia.contains(open));
    {
        // A proxy of a recent project's file stays
        const QString p = makeProxy(unused, 300);
        CHECK(!find(ProxyCleanup::plan(recentMedia), p));
        CHECK(find(ProxyCleanup::plan({open}), p));
        // Deleting a clip's proxy in the Media Pool takes its note along
        ProxyManager pm;
        pm.remove({unused});
        CHECK(!QFileInfo::exists(p));
        CHECK(!QFileInfo::exists(ProxyCleanup::notePath(p)));
    }
    // Sizes in the UI language
    CHECK(!ProxyCleanup::sizeText(3LL * 1024 * 1024 * 1024 / 2).isEmpty());
    CHECK(ProxyCleanup::sizeText(3LL * 1024 * 1024 * 1024 / 2).contains("1"));
    for (const auto r : {ProxyCleanup::Reason::Missing, ProxyCleanup::Reason::Changed, ProxyCleanup::Reason::OtherSize,
                         ProxyCleanup::Reason::Unused, ProxyCleanup::Reason::Unknown, ProxyCleanup::Reason::Leftover}) {
        CHECK(!ProxyCleanup::reasonText(r).isEmpty());
        CHECK(qstrlen(ProxyCleanup::reasonId(r)) > 0);
    }

    testDialog(tmp.path());
    QDir(ProxyManager::cacheDir()).removeRecursively();
    return Check::result();
}
