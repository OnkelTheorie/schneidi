// Green Screen pipette (ui/KeyColorPicker, Inspector button "KeyPipette", MainWindow::startKeyPick): isolate() leaves
// one clip as its key sees it (fixed stages before the key, no fades/opacity, other tracks empty), sample() averages the
// covered pixels; in the main window the button shows the clip without its key in the viewer, a click there sets
// the key color (one undo step) and gives the viewer back to the Color page; right-click, Esc and another selection
// cancel without changing anything.
// KEY_PICK_DUMP=<png>: picture of the viewer while the pipette is on.

#include "check.h"

#include "app/MainWindow.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/KeyColorPicker.h"
#include "ui/Viewer.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>
#include <clocale>

namespace {

void testSample()
{
    QImage img(10, 10, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 5; ++x) img.setPixelColor(x, y, QColor(0, 200, 40));
    img.setPixelColor(4, 4, QColor(0, 100, 40)); // one darker pixel in the middle of the square
    // 3x3 around (4,4): six covered pixels (x 3..4), the transparent ones (x 5) do not count
    const QColor c = KeyColorPicker::sample(img, QPointF(4.5, 4.5), 1);
    CHECK(c.isValid());
    CHECK_EQ(c.red(), 0);
    CHECK_EQ(c.green(), (200 * 5 + 100 + 3) / 6);
    CHECK_EQ(c.blue(), 40);
    CHECK(!KeyColorPicker::sample(img, QPointF(8, 5), 1).isValid()); // only transparent pixels
    CHECK(KeyColorPicker::sample(img, QPointF(-1, 0), 1).isValid());  // edge: the part inside the picture
    CHECK(!KeyColorPicker::sample(img, QPointF(-20, 0), 3).isValid());
}

void testIsolate()
{
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(1);
    Clip a;
    a.id = 1;
    a.mediaPath = "a.mp4";
    a.out = 99;
    a.fadeIn = 5;
    a.fadeOut = 7;
    a.transIn = 10;
    a.transform.opacity = 40;
    EffectRegistry::add(a, "blur");
    EffectRegistry::add(a, "chromakey");
    EffectRegistry::add(a, "color");
    EffectRegistry::add(a, "grade");
    Clip other = a;
    other.id = 2;
    other.start = 100;
    tl.video[0].clips = {a, other};
    tl.video[0].hidden = true;
    Clip above = a;
    above.id = 3;
    tl.video[1].clips = {above};
    Clip sound;
    sound.id = 4;
    tl.audio[0].clips = {sound};
    SubtitleTrack st;
    st.cues << SubtitleCue{5, 0, 10, "Hallo"};
    tl.subtitles << st;

    const Timeline alone = KeyColorPicker::isolate(tl, 1);
    if (!CHECK_EQ(alone.video.size(), 2) || !CHECK_EQ(alone.video[0].clips.size(), 1)) return;
    CHECK(alone.video[1].clips.isEmpty() && alone.audio[0].clips.isEmpty() && alone.subtitles[0].cues.isEmpty());
    CHECK(!alone.video[0].hidden);
    const Clip& c = alone.video[0].clips.first();
    CHECK_EQ(c.id, 1);
    // The key sees the graded picture: the grade (a fixed stage before it) stays; the key and the effects, which all
    // render after it whatever their place in the list, go
    CHECK_EQ(c.effects.size(), 1);
    CHECK_EQ(c.effects.value(0).effectId, QString("grade"));
    CHECK(c.transform.opacity == 100 && c.fadeIn == 0 && c.fadeOut == 0 && c.transIn == 0);
    CHECK(KeyColorPicker::isolate(tl, 99).video.isEmpty()); // not in the timeline
    CHECK(KeyColorPicker::isolate(tl, 4).video.isEmpty());  // an audio clip has no picture
}

const Clip* videoClip(Project* p)
{
    const Timeline& tl = p->timeline();
    return tl.video.isEmpty() || tl.video[0].clips.isEmpty() ? nullptr : &tl.video[0].clips.first();
}

QColor keyColor(Project* p)
{
    const Clip* c = videoClip(p);
    return c ? EffectRegistry::value(*c, "chromakey", "color").value<QColor>() : QColor();
}

void testMainWindow(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    // Left half green, right half red
    const QString file = Check::makeMedia(dir + "/screen.mp4",
        {"-f", "lavfi", "-i", "color=c=0x20c040:size=320x180:rate=25:duration=2", "-vf",
         "drawbox=x=160:y=0:w=160:h=180:color=0xd02020@1:t=fill", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p"});
    if (!CHECK(!file.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({file}, true);
    Project* project = w.liveProject();
    auto* viewer = w.findChild<Viewer*>();
    auto* inspector = w.findChild<Inspector*>();
    auto* pipette = w.findChild<QToolButton*>("KeyPipette");
    const Clip* clip = videoClip(project);
    if (!CHECK(viewer && inspector && pipette && clip)) return;
    const int id = clip->id;
    ViewerOverlay* colorOverlay = viewer->overlay();
    CHECK(colorOverlay != nullptr);
    w.liveSelection()->set({id});
    engine.seek(10);
    QApplication::processEvents();

    // Key on the green: in the timeline the left half is gone
    inspector->applyKeyColor(QColor(0x20, 0xc0, 0x40));
    const int steps = project->undoStack()->count();
    {
        const QImage keyed = engine.grabFrame(project->renderTimeline(), 10);
        CHECK(!keyed.isNull() && keyed.pixelColor(60, 90).alpha() < 50 && keyed.pixelColor(260, 90).alpha() > 200);
    }

    // Pipette: the viewer shows the clip without its key
    pipette->click();
    auto* picker = qobject_cast<KeyColorPicker*>(viewer->overlay());
    if (!CHECK(picker)) return;
    CHECK(pipette->isChecked());
    if (const QString dump = qEnvironmentVariable("KEY_PICK_DUMP"); !dump.isEmpty()) {
        viewer->grab().save(dump);
        pipette->parentWidget()->grab().save(QString(dump).replace(".png", "-inspector.png"));
    }
    const QRectF frame(0, 0, 320, 180);
    // A click on the red half sets red (the green one is there to pick even though the key removes it)
    QMouseEvent red(QEvent::MouseButtonPress, QPointF(240, 90), QPointF(240, 90), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
    CHECK(picker->mousePress(&red, frame));
    QApplication::processEvents();
    QColor c = keyColor(project);
    CHECK(c.red() > 170 && c.green() < 70 && c.blue() < 70);
    CHECK_EQ(project->undoStack()->count(), steps + 1);
    CHECK(viewer->overlay() == colorOverlay); // back to the Color page's window outline
    CHECK(!pipette->isChecked());

    pipette->click();
    picker = qobject_cast<KeyColorPicker*>(viewer->overlay());
    if (!CHECK(picker)) return;
    QMouseEvent green(QEvent::MouseButtonPress, QPointF(80, 90), QPointF(80, 90), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    CHECK(picker->mousePress(&green, frame));
    QApplication::processEvents();
    c = keyColor(project);
    CHECK(c.green() > 150 && c.red() < 70 && c.blue() < 100);
    CHECK_EQ(project->undoStack()->count(), steps + 2);
    const Clip* now = videoClip(project);
    CHECK(now && EffectRegistry::instance(*now, "chromakey")->enabled);

    // Right-click, Esc, another selection and the button again cancel without a change
    const QColor before = keyColor(project);
    pipette->click();
    picker = qobject_cast<KeyColorPicker*>(viewer->overlay());
    QMouseEvent right(QEvent::MouseButtonPress, QPointF(240, 90), QPointF(240, 90), Qt::RightButton, Qt::RightButton,
                      Qt::NoModifier);
    if (CHECK(picker)) picker->mousePress(&right, frame);
    QApplication::processEvents();
    CHECK(viewer->overlay() == colorOverlay && !pipette->isChecked());
    pipette->click();
    CHECK(qobject_cast<KeyColorPicker*>(viewer->overlay()));
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(viewer, &esc);
    QApplication::processEvents();
    CHECK(viewer->overlay() == colorOverlay && !pipette->isChecked());
    pipette->click();
    CHECK(qobject_cast<KeyColorPicker*>(viewer->overlay()));
    w.liveSelection()->clear();
    QApplication::processEvents();
    CHECK(viewer->overlay() == colorOverlay && !pipette->isChecked());
    w.liveSelection()->set({id});
    pipette->click();
    CHECK(qobject_cast<KeyColorPicker*>(viewer->overlay()));
    pipette->click();
    QApplication::processEvents();
    CHECK(viewer->overlay() == colorOverlay && !pipette->isChecked());
    CHECK(keyColor(project) == before);
    CHECK_EQ(project->undoStack()->count(), steps + 2);

    // Nothing selected: the button does not stay on
    w.liveSelection()->clear();
    QApplication::processEvents();
    pipette->click();
    CHECK(!pipette->isChecked() && viewer->overlay() == colorOverlay);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("key-pick");
    QTemporaryDir dir;
    testSample();
    testIsolate();
    testMainWindow(dir.path());
    return Check::result();
}
