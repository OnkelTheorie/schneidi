// Test "Paste Attributes" (Alt+V, DaVinci Paste Attributes): chosen properties including keyframes from the copied
// clip onto the selection, video/audio separate, own color correction stays with "Effects", speed with ripple,
// one undo step; dialog (grayed-out checkmarks, remembered choice).
#include "check.h"

#include "app/PasteAttributesDialog.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QCheckBox>
#include <QSettings>
#include <QUndoStack>

namespace {

Clip clip(int id, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.mp4";
    c.start = start;
    c.out = length - 1;
    return c;
}

EffectInstance fx(const QString& id, double v)
{
    EffectInstance e;
    e.effectId = id;
    e.params["v"] = v;
    return e;
}

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, true, false});
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        Clip src = clip(1, 0, 100);
        src.transform.zoomX = src.transform.zoomY = 2;
        src.transform.posX = 100;
        src.transform.rotation = 10;
        src.transform.cropLeft = 50;
        src.transform.opacity = 50;
        Keys::setKey(src, AnimParam::PosX, 10, 0);
        Keys::setKey(src, AnimParam::PosX, 50, 200);
        src.effects << fx("blur", 1) << fx("grade", 1) << fx("chromakey", 1);
        src.fadeIn = 10;
        src.fadeOut = 300; // longer than the target -> limited
        src.speed = 2;
        Clip dst = clip(2, 100, 100);
        dst.transform.rotation = 45;
        dst.effects << fx("grade", 2) << fx("old", 2);
        Keys::setKey(dst, AnimParam::ZoomX, 5, 3); // source has no zoom keyframes -> removed
        Clip asrc = clip(3, 0, 100);
        asrc.volumeDb = -6;
        asrc.pan = 30;
        asrc.fadeIn = 7;
        Clip adst = clip(4, 100, 100);
        Clip later = clip(5, 200, 50); // moves up on speed change
        tl.video[0].clips << src << dst << later;
        tl.audio[0].clips << asrc << adst;
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 10, 0});
    }
    const Clip& at(int id) const { return *TimelineOps::findClip(project.timeline(), id); }
};

void testPaste()
{
    Fixture f;
    CHECK_EQ(f.editor.pasteAttributesAvailable(), 0);
    f.sel.set({1, 3});
    f.editor.copySelection();
    const int all = f.editor.pasteAttributesAvailable();
    CHECK((all & Editor::AttrVideoMask) == Editor::AttrVideoMask && (all & Editor::AttrAudioMask) == Editor::AttrAudioMask);
    CHECK((all & Editor::AttrSpeed) && (all & Editor::AttrFades));
    CHECK_EQ(f.editor.pasteAttributesSource(), QString("a.mp4"));

    f.sel.set({2, 4});
    const Timeline before = f.project.timeline();
    const int undo0 = f.project.undoStack()->count();
    f.editor.pasteAttributes(Editor::AttrZoom | Editor::AttrPosition | Editor::AttrEffects | Editor::AttrVolume |
                             Editor::AttrFades);
    CHECK_EQ(f.project.undoStack()->count(), undo0 + 1);
    const Clip& d = f.at(2);
    CHECK(d.transform.zoomX == 2 && d.transform.zoomY == 2 && d.transform.posX == 100);
    CHECK_EQ(d.transform.rotation, 45.0); // not checked
    CHECK_EQ(d.transform.opacity, 100.0);
    CHECK(d.keys.value(AnimParam::PosX) == f.at(1).keys.value(AnimParam::PosX));
    CHECK(!d.keys.contains(AnimParam::ZoomX));
    // Effects of the source, own color correction stays at the position of the source correction
    CHECK_EQ(d.effects.size(), 3);
    CHECK(d.effects.size() == 3 && d.effects[0].effectId == "blur" && d.effects[1] == fx("grade", 2) &&
          d.effects[2].effectId == "chromakey");
    CHECK(d.fadeIn == 10 && d.fadeOut == 100);
    const Clip& a = f.at(4);
    CHECK(a.volumeDb == -6 && a.pan == 0);
    CHECK_EQ(a.fadeIn, 7); // audio fades from the audio source
    CHECK(a.transform == ClipTransform{}); // video attributes not onto audio clips
    CHECK_EQ(f.at(5).start, 200);

    // Color correction alone: source correction replaces the own, other effects stay
    f.editor.pasteAttributes(Editor::AttrColor | Editor::AttrRotation | Editor::AttrPan);
    CHECK(f.at(2).effects.size() == 3 && f.at(2).effects[1] == fx("grade", 1));
    CHECK(f.at(2).transform.rotation == 10 && f.at(4).pan == 30);

    // Speed: length follows (ripple), one undo step even with two edits
    const int undo1 = f.project.undoStack()->count();
    f.editor.pasteAttributes(Editor::AttrSpeed | Editor::AttrComposite);
    CHECK_EQ(f.project.undoStack()->count(), undo1 + 1);
    CHECK(f.at(2).speed == 2 && f.at(2).length() == 50 && f.at(2).transform.opacity == 50);
    CHECK_EQ(f.at(5).start, 150);
    f.project.undoStack()->undo();
    CHECK(f.at(2).speed == 1 && f.at(2).length() == 100 && f.at(5).start == 200);
    f.project.undoStack()->undo();
    f.project.undoStack()->undo();
    CHECK(f.project.timeline().video[0].clips == before.video[0].clips);
    CHECK(f.project.timeline().audio[0].clips == before.audio[0].clips);

    // Only audio copied: video attributes not available, fades/speed are
    f.sel.set({3});
    f.editor.copySelection();
    const int audioOnly = f.editor.pasteAttributesAvailable();
    CHECK(!(audioOnly & Editor::AttrVideoMask) && (audioOnly & Editor::AttrFades) && (audioOnly & Editor::AttrVolume));
    f.sel.set({2});
    f.editor.pasteAttributes(Editor::AttrZoom | Editor::AttrFades);
    CHECK(f.at(2).transform.zoomX == 1 && f.at(2).fadeIn == 7); // fades fall back to the other source
}

void testDialog()
{
    QSettings().remove("edit/pasteAttributes"); // test run before
    const int avail = Editor::AttrAudioMask | Editor::AttrFades;
    {
        PasteAttributesDialog dlg(avail, "a.mp4", 2);
        CHECK_EQ(dlg.attributes(), 0); // default: nothing checked
        for (QCheckBox* cb : dlg.findChildren<QCheckBox*>())
            if (cb->isEnabled()) cb->setChecked(true);
        CHECK_EQ(dlg.attributes(), avail);
        dlg.accept();
    }
    // remembered; grayed-out ones stay off
    PasteAttributesDialog dlg(Editor::AttrVideoMask | Editor::AttrVolume, "a.mp4", 1);
    CHECK_EQ(dlg.attributes(), int(Editor::AttrVolume));
    const QPixmap shot = dlg.grab();
    CHECK(!shot.isNull());
    if (const QString dump = qEnvironmentVariable("PASTE_DUMP"); !dump.isEmpty()) shot.save(dump);
}

// Color page grade copy/paste and "=" / "-" (grade of the clip one/two before), other effects stay
void testGrade()
{
    Fixture f;
    const auto gradeV = [&](int id) {
        for (const EffectInstance& e : f.at(id).effects)
            if (e.effectId == "grade") return e.params.value("v").toDouble();
        return -1.0;
    };
    CHECK(!f.editor.hasCopiedGrade());
    CHECK(!f.editor.copyGrade(3)); // audio clip
    CHECK(f.editor.copyGrade(1));
    const int undo0 = f.project.undoStack()->count();
    f.editor.pasteGrade({2, 5});
    CHECK_EQ(f.project.undoStack()->count(), undo0 + 1);
    CHECK_EQ(gradeV(2), 1.0);
    CHECK_EQ(gradeV(5), 1.0);
    CHECK_EQ(f.at(2).effects.size(), 2); // "old" stays
    CHECK_EQ(f.at(5).effects.size(), 1);
    CHECK_EQ(f.at(1).effects.size(), 3); // source unchanged
    f.project.undoStack()->undo();
    CHECK_EQ(gradeV(2), 2.0);
    CHECK_EQ(gradeV(5), -1.0);

    // "=": clip 5 gets the grade of clip 2, "-": that of clip 1; the first clip has none before it
    CHECK(f.editor.applyGradeFromPrevious(5, 1));
    CHECK_EQ(gradeV(5), 2.0);
    CHECK(f.editor.applyGradeFromPrevious(5, 2));
    CHECK_EQ(gradeV(5), 1.0);
    CHECK(!f.editor.applyGradeFromPrevious(1, 1));
    CHECK(!f.editor.applyGradeFromPrevious(2, 2));
    // a clip before without a grade resets it
    CHECK(f.editor.copyGrade(5));
    f.project.undoStack()->undo();
    f.project.undoStack()->undo();
    CHECK_EQ(gradeV(5), -1.0);
    CHECK(f.editor.applyGradeFromPrevious(2, 1)); // clip 1 has a grade -> 1
    CHECK_EQ(gradeV(2), 1.0);
}

} // namespace

// Keyframes are stored as source frames (in + clip frame): pasted onto a clip with another in point they keep their
// place in the clip (a zoom from the clip start of A was behind the end of B)
void testKeysFollowInPoint()
{
    Fixture f;
    Timeline tl = f.project.timeline();
    Clip& a = tl.video[0].clips[0];
    a.in = 500;
    a.out = 599;
    a.speed = 1;
    a.keys.clear();
    Keys::setKey(a, AnimParam::ZoomX, 0, 1);
    Keys::setKey(a, AnimParam::ZoomX, 50, 2);
    Keys::setKey(a, AnimParam::GradeExposure, 20, 1);
    Clip& b = tl.video[0].clips[1];
    b.in = 0;
    b.out = 99;
    b.keys.clear();
    tl.audio[0].clips[0].in = 300;
    tl.audio[0].clips[0].out = 399;
    Keys::setKey(tl.audio[0].clips[0], AnimParam::Volume, 10, -12);
    f.project.load(ProjectData{ProjectFormat{}, 0, f.project.media(), {}, tl, 10, 0});
    const auto keysOf = [&](int id, AnimParam p) {
        QVector<int> out;
        for (const Keyframe& k : f.at(id).keys.value(p)) out << k.frame - f.at(id).in;
        return out;
    };
    f.sel.set({1, 3});
    f.editor.copySelection();
    f.sel.set({2, 4});
    f.editor.pasteAttributes(Editor::AttrZoom | Editor::AttrColor | Editor::AttrVolume);
    CHECK(keysOf(2, AnimParam::ZoomX) == (QVector<int>{0, 50}));
    CHECK_EQ(Keys::valueAt(f.at(2), AnimParam::ZoomX, 25), 1.5);
    CHECK(keysOf(2, AnimParam::GradeExposure) == (QVector<int>{20}));
    CHECK(keysOf(4, AnimParam::Volume) == (QVector<int>{10}));
    f.project.undoStack()->undo();
    // Paste grade and grade from the previous clip likewise
    CHECK(f.editor.copyGrade(1));
    f.editor.pasteGrade({2});
    CHECK(keysOf(2, AnimParam::GradeExposure) == (QVector<int>{20}));
    f.project.undoStack()->undo();
    CHECK(f.editor.applyGradeFromPrevious(2, 1));
    CHECK(keysOf(2, AnimParam::GradeExposure) == (QVector<int>{20}));
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("paste_attributes");
    I18n::install("de");
    Theme::apply(app);
    testPaste();
    testDialog();
    testGrade();
    testKeysFollowInPoint();
    return Check::result();
}
