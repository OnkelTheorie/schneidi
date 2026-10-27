// Green Screen pipette (ui/KeyColorPicker, Inspector button "KeyPipette", MainWindow::startKeyPick): isolate() leaves
// one clip as its key sees it (fixed stages before the key, no fades/opacity, other tracks empty), sample() averages the
// covered pixels; in the main window the button shows the clip without its key in the viewer, a click there sets
// the key color (one undo step) and gives the viewer back to the Color page; right-click, Esc and another selection
// cancel without changing anything. The loupe beside the cursor shows the pixels of the full-resolution frame (a single
// pixel the scaled viewer picture loses is there, magnified, the one under the cursor in the middle).
// KEY_PICK_DUMP=<png>: picture of the viewer while the pipette is on (and <png>-loupe.png: the loupe on a small viewer).

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
#include <QPainter>
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

void testLoupe()
{
    // A 1920x1080 green frame with one red and one blue pixel side by side
    QImage img(1920, 1080, QImage::Format_RGBA8888);
    img.fill(QColor(0, 200, 40));
    img.setPixelColor(961, 541, QColor(230, 20, 20));
    img.setPixelColor(962, 541, QColor(20, 20, 230));
    const QImage patch = KeyColorPicker::loupe(img, QPointF(961.6, 541.2));
    CHECK_EQ(patch.size(), QSize(11, 11));
    CHECK_EQ(patch.pixelColor(5, 5), QColor(230, 20, 20)); // the pixel under the cursor in the middle
    CHECK_EQ(patch.pixelColor(6, 5), QColor(20, 20, 230));
    CHECK_EQ(patch.pixelColor(0, 0), QColor(0, 200, 40));
    const QImage corner = KeyColorPicker::loupe(img, QPointF(0.5, 0.5)); // outside the picture: transparent
    CHECK(corner.pixelColor(0, 0).alpha() == 0 && corner.pixelColor(5, 5).alpha() == 255);

    // Drawn on a viewer a quarter as large (where these pixels are gone in the scaled picture): magnified 8 times
    KeyColorPicker picker(img);
    QImage view(480, 270, QImage::Format_RGB32);
    view.fill(Qt::black);
    const QRectF frame(0, 0, 480, 270);
    const QPointF cursor(961.5 / 4, 541.5 / 4);
    picker.hover(cursor, frame);
    {
        QPainter p(&view);
        picker.paint(p, frame);
    }
    const QRectF lens = KeyColorPicker::loupeRect(cursor, frame);
    CHECK(frame.contains(lens));
    CHECK_EQ(lens.size(), QSizeF(88, 88));
    if (const QString dump = qEnvironmentVariable("KEY_PICK_DUMP"); !dump.isEmpty())
        view.save(QString(dump).replace(".png", "-loupe.png"));
    auto cellColor = [&](int cx, int cy) { // middle of a magnified pixel
        return view.pixelColor(int(lens.x()) + cx * 8 + 4, int(lens.y()) + cy * 8 + 4);
    };
    auto near = [](const QColor& a, const QColor& b) {
        return std::abs(a.red() - b.red()) < 4 && std::abs(a.green() - b.green()) < 4 && std::abs(a.blue() - b.blue()) < 4;
    };
    CHECK(near(cellColor(5, 5), QColor(230, 20, 20)));
    CHECK(near(cellColor(6, 5), QColor(20, 20, 230)));
    CHECK(near(cellColor(2, 8), QColor(0, 200, 40)));
    // The whole magnified pixel is the one color (no smoothing), the one under the cursor framed
    const int x0 = int(lens.x()) + 40, y0 = int(lens.y()) + 40;
    bool solid = true;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) solid &= near(view.pixelColor(x0 + x, y0 + y), QColor(230, 20, 20));
    CHECK(solid);
    CHECK(view.pixelColor(x0 - 1, y0 + 4) == QColor(Qt::black) && view.pixelColor(x0 - 2, y0 + 4) == QColor(Qt::white));
    // The color square beside it: the average a click takes (7x7 around the cursor)
    const QColor avg = KeyColorPicker::sample(img, QPointF(961.5, 541.5));
    CHECK(near(view.pixelColor(int(lens.right()) + 4 + 15, int(lens.bottom()) - 15), avg));
    // At the picture's corner the pixels outside it show a checkerboard
    picker.hover(QPointF(0.2, 0.2), frame);
    {
        QPainter p(&view);
        picker.paint(p, frame);
    }
    const QRectF cornerLens = KeyColorPicker::loupeRect(QPointF(0.2, 0.2), frame);
    const QColor outside = view.pixelColor(int(cornerLens.x()) + 6, int(cornerLens.y()) + 6);
    CHECK(outside.red() == outside.green() && outside.green() == outside.blue()
          && (outside.red() == 70 || outside.red() == 110));
    CHECK(near(view.pixelColor(int(cornerLens.x()) + 5 * 8 + 4, int(cornerLens.y()) + 5 * 8 + 4), QColor(0, 200, 40)));
    // Near the right and bottom edge it flips to the other side of the cursor
    const QRectF flipped = KeyColorPicker::loupeRect(QPointF(470, 260), frame);
    CHECK(flipped.right() < 470 && flipped.bottom() < 260);
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
    testLoupe();
    testIsolate();
    testMainWindow(dir.path());
    return Check::result();
}
