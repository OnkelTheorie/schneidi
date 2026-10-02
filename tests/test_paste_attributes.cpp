// Test „Attribute einfügen“ (Alt+V, DaVinci Paste Attributes): gewählte Eigenschaften samt Keyframes vom kopierten
// Clip auf die Auswahl, Video/Audio getrennt, eigene Farbkorrektur bleibt bei „Effekte“, Tempo mit Ripple,
// ein Undo-Schritt; Dialog (ausgegraute Häkchen, gemerkte Wahl).
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
        src.fadeOut = 300; // länger als das Ziel -> begrenzt
        src.speed = 2;
        Clip dst = clip(2, 100, 100);
        dst.transform.rotation = 45;
        dst.effects << fx("grade", 2) << fx("old", 2);
        Keys::setKey(dst, AnimParam::ZoomX, 5, 3); // Quelle hat keine Zoom-Keyframes -> entfernt
        Clip asrc = clip(3, 0, 100);
        asrc.volumeDb = -6;
        asrc.pan = 30;
        asrc.fadeIn = 7;
        Clip adst = clip(4, 100, 100);
        Clip later = clip(5, 200, 50); // rückt bei Tempo-Änderung nach
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
    CHECK_EQ(d.transform.rotation, 45.0); // nicht angehakt
    CHECK_EQ(d.transform.opacity, 100.0);
    CHECK(d.keys.value(AnimParam::PosX) == f.at(1).keys.value(AnimParam::PosX));
    CHECK(!d.keys.contains(AnimParam::ZoomX));
    // Effekte der Quelle, eigene Farbkorrektur bleibt an der Stelle der Quell-Korrektur
    CHECK_EQ(d.effects.size(), 3);
    CHECK(d.effects.size() == 3 && d.effects[0].effectId == "blur" && d.effects[1] == fx("grade", 2) &&
          d.effects[2].effectId == "chromakey");
    CHECK(d.fadeIn == 10 && d.fadeOut == 100);
    const Clip& a = f.at(4);
    CHECK(a.volumeDb == -6 && a.pan == 0);
    CHECK_EQ(a.fadeIn, 7); // Audio-Fades von der Audio-Quelle
    CHECK(a.transform == ClipTransform{}); // Video-Attribute nicht auf Audioclips
    CHECK_EQ(f.at(5).start, 200);

    // Farbkorrektur allein: Quell-Korrektur ersetzt die eigene, übrige Effekte bleiben
    f.editor.pasteAttributes(Editor::AttrColor | Editor::AttrRotation | Editor::AttrPan);
    CHECK(f.at(2).effects.size() == 3 && f.at(2).effects[1] == fx("grade", 1));
    CHECK(f.at(2).transform.rotation == 10 && f.at(4).pan == 30);

    // Tempo: Länge folgt (Ripple), ein Undo-Schritt auch mit zwei Bearbeitungen
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

    // Nur Audio kopiert: Video-Attribute nicht verfügbar, Fades/Tempo schon
    f.sel.set({3});
    f.editor.copySelection();
    const int audioOnly = f.editor.pasteAttributesAvailable();
    CHECK(!(audioOnly & Editor::AttrVideoMask) && (audioOnly & Editor::AttrFades) && (audioOnly & Editor::AttrVolume));
    f.sel.set({2});
    f.editor.pasteAttributes(Editor::AttrZoom | Editor::AttrFades);
    CHECK(f.at(2).transform.zoomX == 1 && f.at(2).fadeIn == 7); // Fades fallen auf die andere Quelle zurück
}

void testDialog()
{
    QSettings().remove("edit/pasteAttributes"); // Testlauf davor
    const int avail = Editor::AttrAudioMask | Editor::AttrFades;
    {
        PasteAttributesDialog dlg(avail, "a.mp4", 2);
        CHECK_EQ(dlg.attributes(), 0); // Vorgabe: nichts angehakt
        for (QCheckBox* cb : dlg.findChildren<QCheckBox*>())
            if (cb->isEnabled()) cb->setChecked(true);
        CHECK_EQ(dlg.attributes(), avail);
        dlg.accept();
    }
    // gemerkt; ausgegraute bleiben aus
    PasteAttributesDialog dlg(Editor::AttrVideoMask | Editor::AttrVolume, "a.mp4", 1);
    CHECK_EQ(dlg.attributes(), int(Editor::AttrVolume));
    const QPixmap shot = dlg.grab();
    CHECK(!shot.isNull());
    if (const QString dump = qEnvironmentVariable("PASTE_DUMP"); !dump.isEmpty()) shot.save(dump);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("paste_attributes");
    I18n::install("de");
    Theme::apply(app);
    testPaste();
    testDialog();
    return Check::result();
}
