// SPDX-License-Identifier: AGPL-3.0-only
// Removing clips from the Media Pool (Project::removeMedia, edit op 'remove_media', Delete/Backspace and the context
// menu in the pool): unused media go in one undo step, used ones only with their clips (every timeline, compound clips
// included) after a question that names them; undo brings back exactly what was there (place in the pool, bin, color,
// flags, marks, transcript, clips); a watched file does not come back through its watch; an AI chat turn combined into
// one step keeps the removal (and a watch import).
#include "check.h"

#include "app/InputBindings.h"
#include "cli/Commands.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/MediaPool.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUndoStack>
#include <functional>

namespace {

const QString A = "/x/a.mp4", B = "/x/b.mp4", C = "/x/c.mp4", D = "/x/d.mp4";

MediaInfo media(const QString& path)
{
    return MediaInfo{path, QFileInfo(path).fileName(), 100, true, true, false};
}

Clip clip(int id, const QString& path, int start, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.out = 24;
    c.linkId = link;
    return c;
}

// Timeline 1: a (video + linked audio), b with a dissolve to a; Timeline 2: a; Compound Clip: c. d unused.
ProjectData sample()
{
    ProjectData d;
    d.media = {media(A), media(B), media(C), media(D)};
    d.bins = {MediaBin{1, 0, "Aufnahmen"}};
    d.media[1].bin = 1;
    d.media[1].clipColor = "orange";
    d.media[1].flags = {"red"};
    d.media[1].markIn = 5;
    d.media[1].markOut = 40;
    d.media[1].markers = {12};
    d.media[1].transcripts = {MediaTranscript{0, "de", {{0, 500, "Hallo"}}}};
    Timeline t1 = emptyTimeline();
    Clip b = clip(3, B, 25);
    Clip a = clip(1, A, 50, 1);
    b.transOut = 10;
    a.transIn = 10;
    a.in = 20;
    a.out = 44;
    t1.video[0].clips << b << a;
    Clip aa = a;
    aa.id = 2;
    t1.audio[0].clips << aa;
    Timeline t2 = emptyTimeline();
    t2.video[0].clips << clip(4, A, 0);
    Timeline t3 = emptyTimeline();
    t3.video[0].clips << clip(5, C, 0);
    t1.video[1].clips << [] {
        Clip nested;
        nested.id = 6;
        nested.kind = ClipKind::Compound;
        nested.sequenceId = 3;
        nested.start = 100;
        nested.out = 24;
        return nested;
    }();
    d.sequences = {Sequence{1, "Timeline 1", false, 0, t1}, Sequence{2, "Timeline 2", false, 0, t2},
                   Sequence{3, "Compound Clip 1", true, 0, t3}};
    d.timeline = t1;
    d.currentSequence = 1;
    d.lastClipId = 10;
    d.lastLinkId = 1;
    return d;
}

QByteArray json(const Project& p) { return ProjectFile::toJson(p.data(), "/x/p.schneidi"); }

QStringList paths(const Project& p)
{
    QStringList out;
    for (const MediaInfo& m : p.media()) out << m.path;
    return out;
}

int clipsOf(const Project& p, const QString& path)
{
    int n = 0;
    for (const auto& [id, clips] : p.mediaUsage({path})) n += clips;
    return n;
}

bool waitUntil(const std::function<bool()>& ok, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < ms) QTest::qWait(10);
    return ok();
}

void testCore()
{
    Project p;
    p.load(sample());
    QUndoStack* undo = p.undoStack();
    const QByteArray start = json(p);

    // Who uses what: per timeline, compound clips included
    CHECK_EQ(p.mediaUsage({A}), (QVector<QPair<int, int>>{{1, 2}, {2, 1}}));
    CHECK_EQ(p.mediaUsage({C}), (QVector<QPair<int, int>>{{3, 1}}));
    CHECK(p.mediaUsage({D}).isEmpty());

    // Used and not asked for the clips: nothing happens
    CHECK(!p.removeMedia({A, D}, false));
    CHECK_EQ(json(p), start);
    CHECK_EQ(undo->count(), 0);
    CHECK(!p.removeMedia({"/x/nicht-im-pool.mp4"}, true));
    CHECK_EQ(undo->count(), 0);

    // Unused: one step, back at its place
    CHECK(p.removeMedia({D}, false));
    CHECK_EQ(paths(p), (QStringList{A, B, C}));
    CHECK_EQ(undo->count(), 1);
    CHECK_EQ(undo->undoText(), T("„%1“ aus dem Media Pool entfernen").arg("d.mp4"));
    CHECK(p.isModified());
    undo->undo();
    CHECK_EQ(json(p), start);
    CHECK(!p.isModified());

    // Used ones with their clips: every timeline, one step, a gap stays, the dissolve partner keeps no half dissolve
    CHECK(p.removeMedia({A, B, C}, true));
    CHECK_EQ(paths(p), QStringList{D});
    CHECK_EQ(undo->count(), 1);
    CHECK_EQ(undo->undoText(), T("%1 Clips aus dem Media Pool entfernen").arg(3));
    CHECK_EQ(clipsOf(p, A) + clipsOf(p, B) + clipsOf(p, C), 0);
    for (const Sequence& s : p.sequences())
        for (const auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips) CHECK(c.kind != ClipKind::Media || c.mediaPath == D);
    CHECK(TimelineOps::findClip(p.timeline(), 6)); // the compound clip stays (now empty)
    CHECK_EQ(p.currentSequence(), 1);
    undo->undo();
    CHECK_EQ(json(p), start); // order, bin, color, flags, marks, transcript, clips
    undo->redo();
    CHECK_EQ(paths(p), QStringList{D});

    // What changed outside the history while it was back (source marks) stays when it comes back again
    undo->undo();
    p.setMediaMarks(B, 7, 9);
    undo->redo();
    undo->undo();
    CHECK(p.mediaInfo(B) && p.mediaInfo(B)->markIn == 7 && p.mediaInfo(B)->markOut == 9);
    CHECK_EQ(paths(p), (QStringList{A, B, C, D}));
    // A file imported meanwhile stays where it is
    p.addMedia(media("/x/e.mp4"));
    undo->redo();
    CHECK_EQ(paths(p), (QStringList{D, "/x/e.mp4"}));
    undo->undo();
    CHECK_EQ(paths(p), (QStringList{A, B, C, D, "/x/e.mp4"}));
}

// A watched folder's file goes on its ignored list (the watch does not bring it back); undo takes it off again
void testWatched()
{
    Project p;
    ProjectData d = sample();
    d.media << media("/w/neu.mp4") << media("/w/unten/tief.mp4");
    d.watchFolders = {WatchFolder{"/w", false, true, true, true, 0, {}, {"/w/alt.mp4"}}};
    p.load(d);
    CHECK_EQ(WatchFolders::covering(p.watchFolders(), "/w/neu.mp4"), 0);
    CHECK_EQ(WatchFolders::covering(p.watchFolders(), "/w/unten/tief.mp4"), -1); // no subfolders
    CHECK_EQ(WatchFolders::covering(p.watchFolders(), "/w/text.txt"), -1);
    CHECK_EQ(WatchFolders::covering(p.watchFolders(), "/wx/neu.mp4"), -1);
    int changed = 0;
    QObject::connect(&p, &Project::watchFoldersChanged, [&] { ++changed; });
    CHECK(p.removeMedia({"/w/neu.mp4", "/w/unten/tief.mp4"}, false));
    CHECK_EQ(p.watchFolders().value(0).ignored, (QStringList{"/w/alt.mp4", "/w/neu.mp4"}));
    CHECK_EQ(changed, 1);
    p.undoStack()->undo();
    CHECK_EQ(p.watchFolders().value(0).ignored, QStringList{"/w/alt.mp4"});
    CHECK(p.mediaInfo("/w/neu.mp4") && p.mediaInfo("/w/unten/tief.mp4"));
    // The ignored list is in the file: the project stays modified even with the history back at its start
    CHECK(p.isModified());
    WatchFolder deep = p.watchFolders().value(0);
    deep.subfolders = true;
    CHECK_EQ(WatchFolders::covering({deep}, "/w/unten/tief.mp4"), 0);
}

// An AI chat turn combined into one step (Project::combineSteps) keeps what its steps did to the media list
void testCombined()
{
    Project p;
    ProjectData d = sample();
    d.watchFolders = {WatchFolder{"/w", false, true, true, true, 0, {}, {}}};
    p.load(d);
    const QByteArray start = json(p);
    const int from = p.undoStack()->index();
    CHECK(p.removeMedia({A}, true));
    p.addMedia(media("/x/importiert.mp4")); // an import of the turn is not in the history: it stays either way
    MediaInfo watched = media("/w/neu.mp4");
    CHECK_EQ(p.addWatchedMedia({WatchImport{watched, "/w"}}).size(), 1);
    p.addTimeline("Neu");
    const QStringList after = paths(p);
    CHECK_EQ(after, (QStringList{B, C, D, "/x/importiert.mp4", "/w/neu.mp4"}));
    p.combineSteps(from, "KI-Chat: weg damit");
    CHECK_EQ(p.undoStack()->count(), 1);
    CHECK_EQ(paths(p), after);
    CHECK_EQ(clipsOf(p, A), 0);
    p.undoStack()->undo();
    CHECK_EQ(paths(p), (QStringList{A, B, C, D, "/x/importiert.mp4"}));
    CHECK_EQ(clipsOf(p, A), 3);
    CHECK_EQ(p.watchFolders().value(0).ignored, QStringList{"/w/neu.mp4"}); // like an undone watch import
    p.undoStack()->redo();
    CHECK_EQ(paths(p), after);
    CHECK(p.watchFolders().value(0).ignored.isEmpty());
    Q_UNUSED(start);
}

// schneidi-cli / MCP: edit op 'remove_media'
QString cliError;
QJsonObject edit(const QString& project, const QString& ops)
{
    cliError.clear();
    try {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        return cmd->run(Cli::normalize(*cmd, {{"project", project}, {"ops", ops}}), ctx);
    } catch (const Cli::Error& e) {
        cliError = e.code + ": " + e.message;
    }
    return {};
}

void testCli(const QString& dir)
{
    ProjectData d = sample();
    const QString a = QDir(dir).filePath("a.mp4"), dd = QDir(dir).filePath("d.mp4");
    for (MediaInfo& m : d.media) m.path = QDir(dir).filePath(m.name);
    for (Sequence& s : d.sequences)
        for (auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (Track& t : *tracks)
                for (Clip& c : t.clips)
                    if (c.kind == ClipKind::Media) c.mediaPath = QDir(dir).filePath(QFileInfo(c.mediaPath).fileName());
    d.timeline = d.sequences[0].timeline;
    const QString project = QDir(dir).filePath("p.schneidi");
    QString error;
    if (!CHECK(ProjectFile::save(d, project, &error))) return;
    const auto load = [&] {
        ProjectData back;
        ProjectFile::load(project, &back, &error);
        return back;
    };

    // Used: a clear error that names the timelines, nothing saved
    edit(project, QString(R"([{"op":"remove_media","media":"%1"}])").arg(a));
    CHECK(cliError.startsWith("IN_USE:"));
    CHECK(cliError.contains("Timeline 1") && cliError.contains("Timeline 2") && cliError.contains("3 clip"));
    CHECK_EQ(load().media.size(), 4);
    // Not in the pool
    edit(project, QString(R"([{"op":"remove_media","media":"%1"}])").arg(QDir(dir).filePath("fehlt.mp4")));
    CHECK(cliError.startsWith("NOT_FOUND:"));
    // Unused, and used with with_clips
    const QJsonObject unused = edit(project, QString(R"([{"op":"remove_media","media":["%1"]}])").arg(dd));
    CHECK(cliError.isEmpty());
    CHECK_EQ(load().media.size(), 3);
    // No clip changes: the note is what tells the AI it worked
    CHECK_EQ(unused["notes"].toArray().at(0).toString(), QString("remove_media: d.mp4 out of the Media Pool (not deleted from disk)"));
    const QJsonObject r = edit(project, QString(R"([{"op":"remove_media","media":"%1","with_clips":true}])").arg(a));
    CHECK(cliError.isEmpty());
    const ProjectData back = load();
    CHECK_EQ(back.media.size(), 2);
    int left = 0;
    for (const Sequence& s : back.sequences)
        for (const auto* tracks : {&s.timeline.video, &s.timeline.audio})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips) left += c.mediaPath == a ? 1 : 0;
    CHECK_EQ(left, 0);
    CHECK(QJsonDocument(r).toJson().contains("remove_media"));

    // help lists it, with its details
    const Cli::Command* help = Cli::find("help");
    Cli::Context ctx;
    const QJsonObject h = help->run(Cli::normalize(*help, {{"command", "edit"}, {"op", "remove_media"}}), ctx);
    CHECK(QJsonDocument(h).toJson().contains("with_clips"));
}

QListWidgetItem* itemOf(QListWidget* list, const QString& item)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == item) return list->item(i);
    return nullptr;
}

void select(QListWidget* list, const QStringList& items)
{
    list->clearSelection();
    for (const QString& item : items)
        if (QListWidgetItem* it = itemOf(list, item)) it->setSelected(true);
}

bool sendKey(QWidget* w, int key)
{
    QKeyEvent over(QEvent::ShortcutOverride, key, Qt::NoModifier);
    over.ignore();
    QApplication::sendEvent(w, &over);
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    return over.isAccepted();
}

// Answers the question about used media with the button of this text (empty = Cancel); notes what it said
QString g_question, g_dump;
void answer(const QString& button)
{
    QTimer::singleShot(0, [button] {
        waitUntil([] { return qobject_cast<QMessageBox*>(QApplication::activeModalWidget()) != nullptr; });
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) return;
        g_question = box->text();
        if (!g_dump.isEmpty()) box->grab().save(g_dump);
        for (QAbstractButton* b : box->buttons())
            if (button.isEmpty() ? box->buttonRole(b) == QMessageBox::RejectRole : b->text() == button) return b->click();
    });
}

void testPool()
{
    Project p;
    p.load(sample());
    Engine engine; // timeline pictures in the pool open the clips through it
    QString err;
    if (!CHECK(engine.init(&err))) return;
    QWidget window;
    MediaPool pool(&p, &engine, &window);
    pool.setSaveSettings(false);
    InputBindings& b = InputBindings::instance();
    for (const MediaPool::KeyAction& k : pool.keyActions())
        b.registerScopedAction(k.action, k.id, T("Media Pool"), k.defaultKey, k.scope, k.defaultKey2);
    CHECK_EQ(b.inputs("pool_remove_clips"), QStringList({"Del", "Backspace"}));
    window.resize(700, 400);
    pool.resize(700, 400);
    window.show();
    QApplication::processEvents();
    auto* list = pool.findChild<QListWidget*>();
    if (!CHECK(list)) return;
    QUndoStack* undo = p.undoStack();
    const QByteArray start = json(p);

    // Nothing selected (or only the title entry): nothing happens
    select(list, {MediaPool::TitleItem});
    CHECK(sendKey(list, Qt::Key_Delete));
    CHECK_EQ(undo->count(), 0);

    // Unused: Delete removes it right away, Backspace too
    select(list, {D});
    CHECK(sendKey(list, Qt::Key_Delete));
    CHECK(!p.mediaInfo(D));
    undo->undo();
    select(list, {D});
    CHECK(sendKey(list, Qt::Key_Backspace));
    CHECK(!p.mediaInfo(D));
    undo->undo();
    CHECK_EQ(json(p), start);

    // Used: the question names the clips per timeline; Cancel keeps everything
    pool.setCurrentBin(0);
    select(list, {A, D});
    answer({});
    pool.removeSelected();
    CHECK(g_question.contains("a.mp4") && g_question.contains(T("%1: %2 Clips").arg("Timeline 1").arg(2))
          && g_question.contains(T("%1: 1 Clip").arg("Timeline 2")));
    CHECK_EQ(undo->index(), 0);
    CHECK_EQ(json(p), start);
    // "Remove with Their Clips": media and clips in one step, undo brings all back
    answer(T("Mit ihren Clips entfernen"));
    select(list, {A, D});
    CHECK(sendKey(list, Qt::Key_Delete));
    CHECK(!p.mediaInfo(A) && !p.mediaInfo(D));
    CHECK_EQ(clipsOf(p, A), 0);
    CHECK_EQ(undo->index(), 1);
    undo->undo();
    CHECK_EQ(json(p), start);

    // A timeline selected with a medium only it uses: both go in one step without a question
    g_question.clear();
    select(list, {MediaPool::sequenceItem(2), A});
    answer({}); // would cancel: the medium is still used in Timeline 1
    pool.removeSelected();
    CHECK(!g_question.isEmpty() && !g_question.contains("Timeline 2"));
    CHECK_EQ(undo->index(), 0);
    CHECK_EQ(json(p), start);
    {
        Project q;
        ProjectData d = sample();
        d.sequences[0].timeline.video[0].clips.clear();
        d.sequences[0].timeline.audio[0].clips.clear();
        d.timeline = d.sequences[0].timeline;
        q.load(d);
        MediaPool other(&q, &engine);
        other.setSaveSettings(false);
        auto* l = other.findChild<QListWidget*>();
        if (CHECK(l)) {
            select(l, {MediaPool::sequenceItem(2), A});
            g_question.clear();
            other.removeSelected();
            CHECK(g_question.isEmpty());
            CHECK(!q.sequence(2) && !q.mediaInfo(A));
            CHECK_EQ(q.undoStack()->count(), 1);
            CHECK_EQ(q.undoStack()->undoText(), T("Aus dem Media Pool entfernen"));
            q.undoStack()->undo();
            CHECK(q.sequence(2) && q.mediaInfo(A));
        }
    }

    // POOL_REMOVE_DUMP=<folder>: the question as the user sees it
    if (const QString dump = qEnvironmentVariable("POOL_REMOVE_DUMP"); !dump.isEmpty()) {
        g_dump = QDir(dump).filePath("pool-remove-question.png");
        select(list, {A, B});
        answer({});
        pool.removeSelected();
        g_dump.clear();
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("pool-remove");
    QTemporaryDir dir;
    testCore();
    testWatched();
    testCombined();
    testCli(dir.path());
    testPool();
    return Check::result();
}
