// SPDX-License-Identifier: AGPL-3.0-only
// Text-based editing (Transcript panel): media transcripts placed on the timeline through the clips, cutting
// words and pauses (ripple, one undo step), the panel itself with its bindable cut keys and the transcripts in the
// project file.
#include "check.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/TranscriptCut.h"
#include "ui/TranscriptPanel.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QTest>
#include <QTextEdit>
#include <QUndoStack>
#include <QVBoxLayout>

namespace {

const QString kMedia = "/x/talk.mp4";

MediaTranscript transcript()
{
    MediaTranscript t;
    t.language = "de";
    // 25 fps: Hallo 25, Welt 38, das 65, ist 75, gut. 85, Weiter 150 (frames)
    t.words = {{1000, 1500, "Hallo"}, {1500, 2600, "Welt"}, {2600, 3000, "das"},
               {3000, 3400, "ist"},   {3400, 6000, "gut."}, {6000, 6600, "Weiter"}};
    return t;
}

Clip clip(int id, int start, int in, int out, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = kMedia;
    c.start = start;
    c.in = in;
    c.out = out;
    c.linkId = link;
    return c;
}

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    Fixture()
    {
        MediaInfo m{kMedia, "talk.mp4", 3000, true, true, false};
        m.transcripts << transcript();
        project.addMedia(m);
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(2);
        tl.video[0].clips << clip(1, 0, 0, 299, 1);
        tl.audio[0].clips << clip(2, 0, 0, 299, 1);
        ProjectFormat f;
        f.rate = FrameRate{25, 1};
        project.load(ProjectData{f, 0, project.media(), {}, tl, 10, 1});
    }
    QStringList texts(int track = -1) const
    {
        QStringList out;
        for (const auto& w : TranscriptCut::timelineWords(project.timeline(), project.media(), 25, track)) out << w.text;
        return out;
    }
};

void testWords()
{
    Fixture f;
    auto words = TranscriptCut::timelineWords(f.project.timeline(), f.project.media(), 25);
    CHECK_EQ(words.size(), 6);
    CHECK_EQ(words[0].from, 25);
    CHECK_EQ(words[2].from, 65);
    CHECK_EQ(words[2].to, 75);
    CHECK(words[1].spokenTo <= words[1].to && words[1].spokenTo > words[1].from);
    CHECK_EQ(words[5].clipId, 2);

    // Trimmed copy on A2 later in the timeline: only the words inside it, shifted
    f.project.edit("add", [](Timeline& tl) { tl.audio[1].clips << clip(3, 500, 60, 100); });
    words = TranscriptCut::timelineWords(f.project.timeline(), f.project.media(), 25, 1);
    CHECK_EQ(words.size(), 3);
    CHECK_EQ(words[0].from, 505);
    CHECK_EQ(words[0].text, QString("das"));
    CHECK_EQ(words[2].to, 541); // clip end (gut. lasts up to 6 s)
    CHECK_EQ(f.texts().size(), 9);
    CHECK_EQ(f.texts(0).size(), 6);
    // Same clip twice (A1 and A2 at the same place) -> once
    f.project.edit("dup", [](Timeline& tl) { tl.audio[1].clips = {clip(4, 0, 0, 299)}; });
    CHECK_EQ(f.texts().size(), 6);
    // Retimed clips have no words; missingTranscripts lists sources without a transcript
    f.project.edit("speed", [](Timeline& tl) {
        tl.audio[1].clips[0].speed = 2;
        tl.audio[1].clips[0].mediaPath = "/x/other.wav";
    });
    CHECK_EQ(f.texts(1).size(), 0);
    CHECK(TranscriptCut::missingTranscripts(f.project.timeline(), f.project.media()).isEmpty()); // not in the pool
}

void testRanges()
{
    Fixture f;
    const auto words = TranscriptCut::timelineWords(f.project.timeline(), f.project.media(), 25);
    // "das ist" + the pause up to "gut." (none here)
    CHECK(TranscriptCut::cutRange(words, 2, 3, 25) == qMakePair(65, 85));
    // Last word: up to its spoken end
    const auto last = TranscriptCut::cutRange(words, 5, 5, 25);
    CHECK_EQ(last.first, 150);
    CHECK(last.second > 150 && last.second <= 165);
    // "gut." ends speaking at 98, "Weiter" at 150 -> pause cut leaving 3 + 3 frames
    const auto pauses = TranscriptCut::pauseRanges(words, 25, 6);
    CHECK_EQ(pauses.size(), 1);
    CHECK(pauses.value(0) == qMakePair(101, 147));
    CHECK(TranscriptCut::mergeRanges({{10, 20}, {5, 12}, {30, 40}, {20, 25}, {50, 50}})
          == (QVector<QPair<int, int>>{{5, 25}, {30, 40}}));

    // Several ranges in one undo step, ripple on all tracks
    const int undo0 = f.project.undoStack()->count();
    f.editor.deleteRanges({{65, 85}, {101, 147}}, true, "cut");
    CHECK_EQ(f.project.undoStack()->count(), undo0 + 1);
    CHECK_EQ(f.texts(), (QStringList{"Hallo", "Welt", "gut.", "Weiter"}));
    const auto after = TranscriptCut::timelineWords(f.project.timeline(), f.project.media(), 25);
    CHECK_EQ(after[2].from, 65);
    CHECK_EQ(after[3].from, 150 - 20 - 46);
    int videoLength = 0;
    for (const Clip& c : f.project.timeline().video[0].clips) videoLength += c.length();
    CHECK_EQ(videoLength, 300 - 20 - 46);
    f.project.undoStack()->undo();
    CHECK_EQ(f.texts().size(), 6);
}

void testPanel()
{
    Fixture f;
    TranscriptPanel panel(&f.project, &f.editor);
    panel.resize(400, 500);
    panel.show();
    QCoreApplication::processEvents();
    CHECK_EQ(panel.words().size(), 6);
    int seek = -1;
    QObject::connect(&panel, &TranscriptPanel::seekRequested, [&](int frame) { seek = frame; });
    panel.selectWords(2, 3);
    CHECK(panel.cutSelection());
    QCoreApplication::processEvents();
    CHECK_EQ(seek, 65);
    CHECK_EQ(panel.words().size(), 4);
    CHECK_EQ(panel.words().value(2).text, QString("gut."));
    CHECK(!panel.cutSelection()); // selection is gone with the rebuilt text
    f.project.undoStack()->undo();
    QCoreApplication::processEvents();
    CHECK_EQ(panel.words().size(), 6);
}

// The cut keys are a bindable action ("transcript_cut", scope "transcript"): in the text with words selected they
// cut and the window's action on the same key (the timeline's Delete) stays quiet; without words they do nothing
// (a click into the text clears the selection: the timeline clips must not go); rebinding takes effect at once (also
// in the hint)
void testPanelKeys()
{
    Fixture f;
    QWidget window;
    auto* lay = new QVBoxLayout(&window);
    auto* panel = new TranscriptPanel(&f.project, &f.editor);
    lay->addWidget(panel);
    QAction windowDelete(&window); // like MainWindow's "delete"/"delete_alt" (window shortcuts)
    windowDelete.setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace), QKeySequence("X")});
    windowDelete.setShortcutContext(Qt::WindowShortcut);
    window.addAction(&windowDelete);
    int windowDeletes = 0;
    QObject::connect(&windowDelete, &QAction::triggered, [&] { ++windowDeletes; });
    window.resize(400, 500);
    window.show();
    window.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&window));
    auto* text = panel->findChild<QTextEdit*>();
    if (!CHECK(text)) return;
    text->setFocus();
    QCoreApplication::processEvents();
    CHECK_EQ(panel->cutAction()->shortcuts(),
             QList<QKeySequence>({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}));

    panel->selectWords(2, 3);
    QTest::keyClick(text, Qt::Key_Delete);
    QCoreApplication::processEvents();
    CHECK_EQ(panel->words().size(), 4);
    CHECK_EQ(windowDeletes, 0);
    panel->selectWords(0, 0);
    QTest::keyClick(text, Qt::Key_Backspace);
    QCoreApplication::processEvents();
    CHECK_EQ(panel->words().size(), 3);
    CHECK_EQ(windowDeletes, 0);
    // No words selected: nothing (neither words nor the window's Delete)
    const int steps = f.project.undoStack()->index();
    QTest::keyClick(text, Qt::Key_Delete);
    QTest::keyClick(text, Qt::Key_Backspace);
    QCoreApplication::processEvents();
    CHECK_EQ(windowDeletes, 0);
    CHECK_EQ(panel->words().size(), 3);
    CHECK_EQ(f.project.undoStack()->index(), steps);

    // Rebound (key dialog) to X: X cuts in the text, Delete goes to the window even with words selected
    InputBindings& b = InputBindings::instance();
    b.registerAction(panel->cutAction(), "transcript_cut", T("Transkript"), QKeySequence(Qt::Key_Delete), "transcript",
                     QKeySequence(Qt::Key_Backspace));
    CHECK_EQ(b.inputs("transcript_cut"), QStringList({"Del", "Backspace"}));
    b.setInputs("transcript_cut", {"X", ""});
    CHECK_EQ(panel->cutAction()->shortcuts(), QList<QKeySequence>({QKeySequence("X")}));
    panel->selectWords(0, 0);
    QTest::keyClick(text, Qt::Key_Delete);
    QCoreApplication::processEvents();
    CHECK_EQ(windowDeletes, 1);
    CHECK_EQ(panel->words().size(), 3);
    panel->selectWords(0, 0);
    QTest::keyClick(text, Qt::Key_X);
    QCoreApplication::processEvents();
    CHECK_EQ(panel->words().size(), 2);
    CHECK_EQ(windowDeletes, 1);
    QTest::keyClick(text, Qt::Key_X); // nothing selected: X stays in the text too
    QCoreApplication::processEvents();
    CHECK_EQ(panel->words().size(), 2);
    CHECK_EQ(windowDeletes, 1);
    bool hint = false;
    for (const QLabel* l : panel->findChildren<QLabel*>())
        hint |= l->text() == T("Wörter markieren und %1 drücken schneidet sie heraus. Klick = zum Wort springen.")
                                 .arg(QKeySequence("X").toString(QKeySequence::NativeText));
    CHECK(hint);
    b.resetAll();
    CHECK_EQ(panel->cutAction()->shortcuts(),
             QList<QKeySequence>({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)}));
}

void testProjectFile()
{
    Fixture f;
    // Transcripts: set/replace per stream, saved and loaded with the media
    MediaTranscript other;
    other.audioStream = 1;
    other.words = {{0, 500, "Hi"}};
    f.project.setTranscript(kMedia, other);
    CHECK(f.project.isModified());
    CHECK_EQ(f.project.mediaInfo(kMedia)->transcripts.size(), 2);
    const QByteArray json = ProjectFile::toJson(f.project.data(), "/x/p.schneidi");
    ProjectData d;
    QString error;
    CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &d, &error));
    CHECK_EQ(d.media.size(), 1);
    CHECK(d.media.value(0).transcripts == f.project.mediaInfo(kMedia)->transcripts);
    // Empty words remove a stream's transcript
    f.project.setTranscript(kMedia, MediaTranscript{1, {}, {}});
    CHECK_EQ(f.project.mediaInfo(kMedia)->transcripts.size(), 1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("transcript_cut");
    I18n::install("de");
    Theme::apply(app);
    testWords();
    testRanges();
    testPanel();
    testPanelKeys();
    testProjectFile();
    return Check::result();
}
