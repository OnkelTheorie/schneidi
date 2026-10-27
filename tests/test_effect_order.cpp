// Effect order: Editor::moveEffect/moveEffectBy (movable effects only, fixed stages keep their slots, several clips,
// one undo step, nothing moved = no step, project file keeps the order), the `edit` op 'effect' with move (also via a
// linked partner, errors), the Inspector (fixed stages shown first, Move Up/Down in the header menu, dragging a header
// by its grip or title) and the render: the picture follows the order (blur → pixelate ≠ pixelate → blur, each equal to
// the second effect applied to a still of the first), the Green Screen key is a fixed stage wherever it is in the list.
// Pictures of the Inspector while dragging and with the menu: EFFECT_ORDER_DUMP=<folder> build/tests/test_effect_order
#include "check.h"

#include "app/Theme.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "ui/Inspector.h"

#include <Mlt.h>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <cstring>
#include <functional>

namespace {

QStringList ids(const Clip* c)
{
    QStringList out;
    if (c)
        for (const EffectInstance& e : c->effects) out << e.effectId;
    return out;
}

// ---- Core ----

void testCore(const QString& dir)
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    const QString media = "/x/detail.mp4"; // file does not have to exist
    project.addMedia(MediaInfo{media, "detail.mp4", 300, true, false, false});
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(1);
    auto clipAt = [&](int start, const QStringList& fx) {
        Clip c;
        c.id = project.newClipId();
        c.mediaPath = media;
        c.start = start;
        c.out = 99;
        for (const QString& id : fx) c.effects << EffectInstance{id, {}, true};
        return c;
    };
    // stabilize, chromakey and grade are fixed stages; a missing plugin can be moved like any effect
    const Clip a = clipAt(0, {"stabilize", "blur", "chromakey", "color", "grade", "frei0r.gibtsnicht"});
    const Clip b = clipAt(100, {"color", "blur"});
    const Clip locked = clipAt(0, {"color", "blur"});
    tl.video[0].clips << a << b;
    tl.video[1].clips << locked;
    tl.video[1].locked = true;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, locked.id, 0});
    auto clip = [&](int id) { return TimelineOps::findClip(project.timeline(), id); };
    QUndoStack* undo = project.undoStack();
    int changes = 0;
    QObject::connect(&project, &Project::timelineChanged, [&] { ++changes; });

    CHECK(EffectRegistry::movable("blur") && EffectRegistry::movable("color") && EffectRegistry::movable("frei0r.nope"));
    for (const char* fixed : {"stabilize", "grade", "chromakey", "denoise"}) CHECK(!EffectRegistry::movable(fixed));
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"blur", "color", "frei0r.gibtsnicht"}));
    {
        QStringList rendered;
        for (const EffectInstance* e : EffectRegistry::renderOrder(*clip(a.id))) rendered << e->effectId;
        CHECK_EQ(rendered, (QStringList{"stabilize", "grade", "chromakey", "blur", "color", "frei0r.gibtsnicht"}));
    }

    // To the front: the fixed stages keep their slots in the list
    int steps = undo->count();
    CHECK(editor.moveEffect({a.id}, "color", 0));
    CHECK_EQ(ids(clip(a.id)), (QStringList{"stabilize", "color", "chromakey", "blur", "grade", "frei0r.gibtsnicht"}));
    CHECK_EQ(undo->count(), steps + 1);
    CHECK(changes > 0);
    undo->undo();
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"blur", "color", "frei0r.gibtsnicht"}));
    undo->redo();
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"color", "blur", "frei0r.gibtsnicht"}));
    // Steps, clamping, a missing plugin
    CHECK(editor.moveEffectBy({a.id}, "color", 1));
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"blur", "color", "frei0r.gibtsnicht"}));
    CHECK(editor.moveEffect({a.id}, "blur", 99));
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"color", "frei0r.gibtsnicht", "blur"}));
    CHECK(editor.moveEffect({a.id}, "frei0r.gibtsnicht", 0));
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"frei0r.gibtsnicht", "color", "blur"}));
    CHECK_EQ(ids(clip(a.id)), (QStringList{"stabilize", "frei0r.gibtsnicht", "chromakey", "color", "grade", "blur"}));

    // Nothing moves: no undo step, false
    steps = undo->count();
    const QStringList before = ids(clip(a.id));
    CHECK(!editor.moveEffect({a.id}, "frei0r.gibtsnicht", 0));  // there already
    CHECK(!editor.moveEffectBy({a.id}, "blur", 1));             // last already
    CHECK(!editor.moveEffectBy({a.id}, "frei0r.gibtsnicht", -5)); // first already
    for (const char* fixed : {"grade", "chromakey", "stabilize"}) {
        CHECK(!editor.moveEffect({a.id}, fixed, 3));
        CHECK(!editor.moveEffectBy({a.id}, fixed, -1));
    }
    CHECK(!editor.moveEffect({a.id}, "denoise", 0)); // not on the clip
    CHECK(!editor.moveEffect({}, "blur", 0));
    CHECK_EQ(ids(clip(a.id)), before);
    CHECK_EQ(undo->count(), steps);

    // Several clips, one step: each clip moves by its own order; a locked track stays
    steps = undo->count();
    CHECK(editor.moveEffectBy({a.id, b.id, locked.id}, "blur", -1));
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"frei0r.gibtsnicht", "blur", "color"}));
    CHECK_EQ(EffectRegistry::order(*clip(b.id)), (QStringList{"blur", "color"}));
    CHECK_EQ(EffectRegistry::order(*clip(locked.id)), (QStringList{"color", "blur"}));
    CHECK_EQ(undo->count(), steps + 1);
    // A place counts in each clip's own order
    CHECK(editor.moveEffect({a.id, b.id}, "color", 0));
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"color", "frei0r.gibtsnicht", "blur"}));
    CHECK_EQ(EffectRegistry::order(*clip(b.id)), (QStringList{"color", "blur"}));
    undo->undo();
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"frei0r.gibtsnicht", "blur", "color"}));
    CHECK_EQ(EffectRegistry::order(*clip(b.id)), (QStringList{"blur", "color"}));
    undo->redo();
    // Only clips where it moves change: b is done, a moves alone
    CHECK(!editor.moveEffect({a.id, b.id}, "color", 0));
    const Clip bBefore = *clip(b.id);
    CHECK(editor.moveEffect({a.id, b.id}, "frei0r.gibtsnicht", 0));
    CHECK(*clip(b.id) == bBefore);
    CHECK_EQ(EffectRegistry::order(*clip(a.id)), (QStringList{"frei0r.gibtsnicht", "color", "blur"}));

    // Project file keeps the order
    const QString path = QDir(dir).filePath("order.schneidi");
    QString err;
    ProjectData loaded;
    if (CHECK(ProjectFile::save(project.data(), path, &err)) && CHECK(ProjectFile::load(path, &loaded, &err))) {
        const Clip* la = TimelineOps::findClip(loaded.timeline, a.id);
        const Clip* lb = TimelineOps::findClip(loaded.timeline, b.id);
        CHECK(la && la->effects == clip(a.id)->effects);
        CHECK(lb && lb->effects == clip(b.id)->effects);
        CHECK_EQ(ids(la), ids(clip(a.id)));
    }
}

// ---- CLI ----

QString g_path;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QJsonArray& ops, QString* message = nullptr)
{
    try {
        run("edit", {{"project", g_path}, {"ops", ops}});
    } catch (const Cli::Error& e) {
        if (message) *message = e.message;
        return e.code;
    }
    return "none";
}

QJsonObject edit(const QJsonArray& ops) { return run("edit", {{"project", g_path}, {"ops", ops}}); }

QStringList savedIds(int clipId)
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    return ids(TimelineOps::findClip(d.timeline, clipId));
}

void testCli(const QString& dir)
{
    g_path = QDir(dir).filePath("cli.schneidi");
    int video = 0, audio = 0;
    {
        Project p;
        ProjectData d;
        d.timeline = emptyTimeline();
        p.load(d);
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v;
            v.id = video = p.newClipId();
            v.mediaPath = "/x/a.mp4";
            v.out = 99;
            v.linkId = p.newLinkId();
            Clip a = v;
            a.id = audio = p.newClipId();
            tl.video[0].clips << v;
            tl.audio[0].clips << a;
        });
        QString error;
        if (!CHECK(ProjectFile::save(p.data(), g_path, &error))) return;
    }
    auto op = [&](const QString& effect, const QJsonValue& move, int clipId = 0) {
        return QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clipId ? clipId : video}}, {"effect", effect},
                           {"move", move}};
    };
    edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{video}}, {"effect", "blur"}},
          QJsonObject{{"op", "effect"}, {"clips", QJsonArray{video}}, {"effect", "color"}},
          QJsonObject{{"op", "effect"}, {"clips", QJsonArray{video}}, {"effect", "chromakey"}}});
    CHECK_EQ(savedIds(video), (QStringList{"blur", "color", "chromakey"}));
    // `info` lists them in render order: the key (fixed stage) first
    {
        QStringList listed;
        const QJsonObject info = run("info", {{"project", g_path}});
        for (const QJsonValue& e : info["timeline"]["tracks"][0]["clips"][0]["effects"].toArray())
            listed << e["effect"].toString();
        CHECK_EQ(listed, (QStringList{"chromakey", "blur", "color"}));
    }

    // up: one place earlier; the answer tells the new order
    QJsonObject r = edit({op("color", "up")});
    CHECK_EQ(savedIds(video), (QStringList{"color", "blur", "chromakey"}));
    const QString notes = QString::fromUtf8(QJsonDocument(r["notes"].toArray()).toJson());
    if (!CHECK(notes.contains(QString("clip %1: color, blur").arg(video)))) std::printf("       %s\n", qPrintable(notes));
    // A place (1 = first), last, down
    edit({op("blur", 1)});
    CHECK_EQ(savedIds(video), (QStringList{"blur", "color", "chromakey"}));
    edit({op("blur", "last")});
    CHECK_EQ(savedIds(video), (QStringList{"color", "blur", "chromakey"}));
    edit({op("color", "down")});
    CHECK_EQ(savedIds(video), (QStringList{"blur", "color", "chromakey"}));
    // The audio partner stands for the video clip (like every video op)
    edit({op("color", "first", audio)});
    CHECK_EQ(savedIds(video), (QStringList{"color", "blur", "chromakey"}));
    // Already there: no error, a note
    r = edit({op("color", "first")});
    CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("nothing changed"));
    CHECK_EQ(savedIds(video), (QStringList{"color", "blur", "chromakey"}));

    // Errors name the problem
    QString message;
    CHECK_EQ(errorOf({op("chromakey", "up")}, &message), QString("BAD_ARGUMENT"));
    CHECK(message.contains("fixed stage"));
    CHECK_EQ(errorOf({op("grade", "first")}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf({op("blur", "sideways")}, &message), QString("BAD_ARGUMENT"));
    CHECK(message.contains("'up'"));
    CHECK_EQ(errorOf({op("blur", 0)}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf({op("blur", 1.5)}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf({op("blur", true)}), QString("BAD_ARGUMENT"));
    QJsonObject withParams = op("blur", "up");
    withParams["params"] = QJsonObject{{"strength", 10}};
    CHECK_EQ(errorOf({withParams}, &message), QString("BAD_ARGUMENT"));
    CHECK(message.contains("params"));
    CHECK_EQ(errorOf({op("frei0r.nope", "up")}), QString("NOT_FOUND"));
    QJsonObject noEffect = op("", "up");
    noEffect.remove("effect");
    CHECK_EQ(errorOf({noEffect}), QString("BAD_ARGUMENT"));
    edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{video}}, {"effect", "blur"}, {"remove", true}}});
    CHECK_EQ(errorOf({op("blur", "up")}, &message), QString("NOT_FOUND"));
    CHECK(message.contains("none of the clips"));
    CHECK_EQ(savedIds(video), (QStringList{"color", "chromakey"}));

    // Help: usage and details of the op mention move and the fixed stages
    for (const Cli::detail::OpDef& d : Cli::detail::editOps())
        if (d.name == "effect") {
            CHECK(d.usage.contains("move:'up'|'down'|'first'|'last'|place"));
            CHECK(d.details.contains("Fixed stages"));
        }
    CHECK(Cli::detail::editHelp().contains("move:"));
}

// ---- Inspector ----

QWidget* header(Inspector& insp, const QString& name)
{
    for (QToolButton* b : insp.findChildren<QToolButton*>())
        if (b->isCheckable() && b->text() == name && b->parentWidget() && b->parentWidget()->isVisibleTo(&insp))
            return b->parentWidget();
    return nullptr;
}

QToolButton* titleOf(QWidget* h)
{
    for (QToolButton* b : h->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        if (b->isCheckable() && b->toolTip() == T("Auf-/zuklappen")) return b;
    return nullptr;
}

void send(QWidget* w, QEvent::Type type, const QPoint& global, Qt::MouseButtons buttons)
{
    const QPoint local = w->mapFromGlobal(global);
    QMouseEvent e(type, local, global, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QApplication::sendEvent(w, &e);
}

// Drag from `from` (pressed widget) to a global point; `during` runs while the line shows
void drag(QWidget* from, const QPoint& to, const std::function<void()>& during = {})
{
    const QPoint start = from->mapToGlobal(QPoint(from->width() / 2, from->height() / 2));
    send(from, QEvent::MouseButtonPress, start, Qt::LeftButton);
    send(from, QEvent::MouseMove, start + QPoint(0, 2), Qt::LeftButton); // below the drag distance
    send(from, QEvent::MouseMove, (start + to) / 2, Qt::LeftButton);
    send(from, QEvent::MouseMove, to, Qt::LeftButton);
    if (during) during();
    send(from, QEvent::MouseButtonRelease, to, Qt::NoButton);
}

void testInspector(QApplication& app)
{
    const QString dump = QString::fromLocal8Bit(qgetenv("EFFECT_ORDER_DUMP"));
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(MediaInfo{"/x/b.mp4", "b.mp4", 300, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = project.newClipId();
    c.mediaPath = "/x/b.mp4";
    c.out = 99;
    tl.video[0].clips << c;
    Clip second = c;
    second.id = project.newClipId();
    second.start = 100;
    tl.video[0].clips << second;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, second.id, 0});
    auto order = [&] { return EffectRegistry::order(*TimelineOps::findClip(project.timeline(), c.id)); };

    Inspector insp(&editor);
    insp.resize(380, 900);
    sel.set({c.id, second.id});
    insp.show();
    editor.addEffect({c.id, second.id}, "blur");
    editor.addEffect({c.id, second.id}, "color");
    editor.addEffect({c.id, second.id}, "stabilize"); // fixed stage: added last, shown first
    app.processEvents();
    const QString blurName = EffectRegistry::find("blur")->name, colorName = EffectRegistry::find("color")->name;
    const QString stabName = EffectRegistry::find("stabilize")->name;
    QWidget* hBlur = header(insp, blurName);
    QWidget* hColor = header(insp, colorName);
    QWidget* hStab = header(insp, stabName);
    if (!CHECK(hBlur && hColor && hStab)) return;
    CHECK(hStab->y() < hBlur->y() && hBlur->y() < hColor->y());
    auto grip = [](QWidget* h) { return h->findChild<QLabel*>("EffectGrip"); };
    CHECK(grip(hBlur) && !grip(hBlur)->pixmap().isNull());
    CHECK(grip(hStab) && grip(hStab)->pixmap().isNull()); // only the indent

    // Header menu: Move Up/Down (enabled at the right places), triggers for both selected clips in one step
    auto menu = [&](QWidget* h, const QString& text, bool* enabled, const QString& shot = {}) {
        bool found = false;
        QTimer::singleShot(0, [&] {
            auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!m) return;
            if (!shot.isEmpty()) { // the Inspector with the open menu
                QImage img = insp.grab().toImage();
                QPainter p(&img);
                p.drawPixmap(insp.mapFromGlobal(m->pos()), m->grab());
                p.end();
                img.save(shot);
            }
            for (QAction* a : m->actions())
                if (a->text() == text) {
                    found = true;
                    if (enabled) *enabled = a->isEnabled();
                    if (a->isEnabled()) a->trigger();
                }
            m->close();
        });
        emit h->customContextMenuRequested(QPoint(40, h->height() / 2));
        app.processEvents();
        return found;
    };
    bool enabled = true;
    CHECK(menu(hBlur, T("Nach oben"), &enabled) && !enabled); // first already
    CHECK_EQ(order(), (QStringList{"blur", "color"}));
    const int steps = project.undoStack()->count();
    CHECK(menu(hBlur, T("Nach unten"), &enabled, dump.isEmpty() ? QString() : QDir(dump).filePath("menu.png")) && enabled);
    CHECK_EQ(order(), (QStringList{"color", "blur"}));
    CHECK_EQ(EffectRegistry::order(*TimelineOps::findClip(project.timeline(), second.id)), (QStringList{"color", "blur"}));
    CHECK_EQ(project.undoStack()->count(), steps + 1);
    app.processEvents();
    CHECK(hColor->y() < hBlur->y()); // sections follow
    CHECK(!menu(hStab, T("Nach oben"), nullptr)); // fixed stage: no move entries

    // Drag by the grip: blur above color; the line shows while dragging, the header is highlighted
    bool lineShown = false;
    const QString before = hBlur->styleSheet();
    drag(grip(hBlur), hColor->mapToGlobal(QPoint(30, 2)), [&] {
        app.processEvents();
        auto* line = insp.findChild<QWidget*>("EffectDropLine");
        lineShown = line && line->isVisible() && std::abs(line->y() - hColor->y()) <= 2;
        CHECK(hBlur->styleSheet() != before);
        if (!dump.isEmpty()) insp.grab().save(QDir(dump).filePath("drag.png"));
    });
    CHECK(lineShown);
    CHECK_EQ(order(), (QStringList{"blur", "color"}));
    CHECK_EQ(hBlur->styleSheet(), before);
    app.processEvents();
    CHECK(hBlur->y() < hColor->y());
    // Drag by the title: blur below color (far below = last), the section stays open (no fold on the release)
    QToolButton* blurTitle = titleOf(hBlur);
    if (CHECK(blurTitle)) {
        drag(blurTitle, hColor->mapToGlobal(QPoint(30, 600)));
        CHECK_EQ(order(), (QStringList{"color", "blur"}));
        CHECK(blurTitle->isChecked());
        // A plain click still folds and opens it
        blurTitle->click();
        CHECK(!blurTitle->isChecked());
        blurTitle->click();
        CHECK(blurTitle->isChecked());
    }
    // A drag that ends in its own gap changes nothing
    const int stepsNow = project.undoStack()->count();
    app.processEvents();
    drag(grip(hBlur), hBlur->mapToGlobal(QPoint(40, 1)));
    CHECK_EQ(project.undoStack()->count(), stepsNow);
    CHECK_EQ(order(), (QStringList{"color", "blur"}));
    // The fixed stage cannot be dragged
    if (hStab) {
        drag(hStab, hColor->mapToGlobal(QPoint(30, hColor->height() + 200)));
        CHECK_EQ(project.undoStack()->count(), stepsNow);
    }
    CHECK(!QApplication::overrideCursor());
}

// ---- Render ----

QImage render(const Timeline& tl, const ProjectFormat& fmt, int pos)
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    tr->seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr->get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    int w = prof->width(), h = prof->height();
    const uint8_t* d = f->get_image(ifmt, w, h);
    if (!d) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img.copy();
}

Timeline one(const QString& media, const QVector<EffectInstance>& fx)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = media;
    c.out = 49;
    c.effects = fx;
    tl.video[0].clips << c;
    return tl;
}

// Mean deviation of two images, 0..255
double diff(const QImage& x, const QImage& y)
{
    if (x.isNull() || y.isNull() || x.size() != y.size()) return 1e9;
    const QImage a = x.convertToFormat(QImage::Format_RGB888), b = y.convertToFormat(QImage::Format_RGB888);
    double sum = 0;
    for (int r = 0; r < a.height(); ++r)
        for (int i = 0; i < a.width() * 3; ++i) sum += std::abs(a.scanLine(r)[i] - b.scanLine(r)[i]);
    return sum / (a.width() * a.height() * 3.0);
}

// Hard steps between horizontal neighbours (any channel more than 40 apart), per row
double hardSteps(const QImage& x)
{
    const QImage a = x.convertToFormat(QImage::Format_RGB888);
    long n = 0;
    for (int r = 0; r < a.height(); ++r) {
        const uchar* p = a.scanLine(r);
        for (int i = 3; i < a.width() * 3; ++i) n += std::abs(p[i] - p[i - 3]) > 40;
    }
    return double(n) / a.height();
}

// `first` rendered, saved as a still, `second` applied to the still: what first → second must look like
QImage composed(const QString& media, const EffectInstance& first, const EffectInstance& second,
                const ProjectFormat& fmt, const QString& dir)
{
    const QString still = QDir(dir).filePath(first.effectId + ".png");
    if (!render(one(media, {first}), fmt, 10).save(still)) return {};
    return render(one(still, {second}), fmt, 10);
}

void testRender(const QString& dir)
{
    const QString media = Check::makeMedia(QDir(dir).filePath("bars.mp4"),
                                           {"-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=2", "-vf",
                                            "scale=out_color_matrix=bt709", "-c:v", "libx264", "-preset", "ultrafast",
                                            "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
                                            "-color_trc", "bt709"});
    if (!CHECK(!media.isEmpty())) return;
    ProjectFormat fmt;
    fmt.width = 640;
    fmt.height = 360;
    const EffectInstance blur{"blur", {{"strength", 40.0}}, true};
    const EffectInstance color{"color", {{"contrast", 100.0}}, true};
    // Built-in pair (always there): blur → contrast differs from contrast → blur, each one as composed by hand
    {
        const QImage bc = render(one(media, {blur, color}), fmt, 10), cb = render(one(media, {color, blur}), fmt, 10);
        const QImage wantBc = composed(media, blur, color, fmt, dir), wantCb = composed(media, color, blur, fmt, dir);
        const double apart = diff(bc, cb), okBc = diff(bc, wantBc), okCb = diff(cb, wantCb);
        if (!CHECK(apart > 1.5 && okBc < apart / 3 && okCb < apart / 3))
            std::printf("       blur/color: apart %.2f, bc %.2f, cb %.2f\n", apart, okBc, okCb);
    }
    // Blur and pixelate (frei0r): pixelate last = hard block edges, blur last = none
    if (const EffectDescriptor* pix = EffectRegistry::find("frei0r.pixeliz0r")) {
        EffectInstance pixel{pix->id, {}, true};
        for (const EffectParam& p : pix->params) pixel.params[p.key] = p.type == EffectParam::Double ? 0.1 : p.defaultValue;
        const QImage bp = render(one(media, {blur, pixel}), fmt, 10), pb = render(one(media, {pixel, blur}), fmt, 10);
        const QImage wantBp = composed(media, blur, pixel, fmt, dir), wantPb = composed(media, pixel, blur, fmt, dir);
        const double apart = diff(bp, pb), okBp = diff(bp, wantBp), okPb = diff(pb, wantPb);
        if (!CHECK(apart > 3.0 && okBp < apart / 3 && okPb < apart / 3))
            std::printf("       blur/pixelate: apart %.2f, bp %.2f, pb %.2f\n", apart, okBp, okPb);
        const double stepsBp = hardSteps(bp), stepsPb = hardSteps(pb);
        if (!CHECK(stepsBp > 4 && stepsPb < 0.5)) std::printf("       hard steps: bp %.2f, pb %.2f\n", stepsBp, stepsPb);
        // Through the Editor: moving the effect changes the picture to the other order
        Project project;
        Selection sel;
        Editor editor(&project, &sel);
        project.load(ProjectData{fmt, 0, {}, {}, one(media, {blur, pixel}), 1, 0});
        CHECK(diff(render(project.timeline(), fmt, 10), bp) < 0.5);
        CHECK(editor.moveEffectBy({1}, pix->id, -1));
        CHECK(diff(render(project.timeline(), fmt, 10), pb) < 0.5);
        project.undoStack()->undo();
        CHECK(diff(render(project.timeline(), fmt, 10), bp) < 0.5);
    } else {
        std::printf("Hinweis: frei0r.pixeliz0r fehlt, Blur/Pixelate übersprungen\n");
    }
    // Green Screen: a fixed stage right after the grade, wherever it sits in the list
    if (EffectRegistry::find("chromakey")) {
        const EffectInstance key{"chromakey", {{"color", QColor(0, 191, 0)}, {"distance", 0.3}}, true};
        const QImage first = render(one(media, {key, blur}), fmt, 10), last = render(one(media, {blur, key}), fmt, 10);
        const QImage alone = render(one(media, {blur}), fmt, 10);
        if (!CHECK(diff(first, last) < 0.5 && diff(first, alone) > 1.0))
            std::printf("       key: first/last %.2f, keyed/unkeyed %.2f\n", diff(first, last), diff(first, alone));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("effect-order");
    I18n::install("de");
    Theme::apply(app);
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testCore(tmp.path());
    testCli(tmp.path());
    Check::initMlt();
    testInspector(app);

    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Render-Teil übersprungen\n");
        return Check::result();
    }
    {
        // The MLT Qt module refuses without a display in some builds: then no render part
        auto prof = makeProfile(ProjectFormat{});
        Mlt::Transition qt(*prof, "qtblend");
        if (!qt.is_valid()) {
            std::printf("Hinweis: MLT-Qt-Modul nicht nutzbar, Render-Teil übersprungen\n");
            return Check::result();
        }
    }
    testRender(tmp.path());
    return Check::result();
}
