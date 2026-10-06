// Test frei0r plugins as effects: registry from the MLT metadata (number, checkbox, color, choice; old alias names
// only once), rendering with parameters (pixel values on color bars), saving/loading parameters with their type,
// Inspector (sections appear with the first clip that has the effect, checkbox/choice with undo) and Effects Library.
// Keyframes on frei0r number parameters (dynamic AnimParam per effect + key): values, render (also on a cut that does
// not start at source frame 0), project file (also for a missing plugin), Inspector diamond with undo, removing.
// Missing plugin (project from another computer): listed on load, stays in the file, Inspector shows a warning section
// that can remove it.
// Pictures of Inspector and Library: FREI0R_DUMP=<folder> build-tests/tests/test_frei0r
#include "check.h"

#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "ui/EffectsLibrary.h"
#include "ui/Inspector.h"

#include <Mlt.h>
#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QSet>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <QTemporaryDir>
#include <clocale>
#include <cstring>
#include <functional>

namespace {

QString g_media;

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

Timeline one(const std::function<void(Clip&)>& fn)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = g_media;
    c.out = 49;
    fn(c);
    tl.video[0].clips << c;
    return tl;
}

std::function<void(Clip&)> fx(const QString& id, const QVariantMap& vals = {})
{
    return [=](Clip& c) {
        CHECK(EffectRegistry::add(c, id));
        for (auto it = vals.begin(); it != vals.end(); ++it) EffectRegistry::instance(c, id)->params[it.key()] = it.value();
    };
}

// Mean of the top left (SMPTE bars: 75 % gray)
QColor left(const QImage& img)
{
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = img.height() / 10; y < img.height() / 2; y += 4)
        for (int x = img.width() / 50; x < img.width() / 10; x += 4) {
            const QColor c = img.pixelColor(x, y);
            r += c.red(); g += c.green(); b += c.blue();
            ++n;
        }
    return QColor(int(r / n), int(g / n), int(b / n));
}

const EffectParam* param(const EffectDescriptor* d, EffectParam::Type type)
{
    for (const EffectParam& p : d->params)
        if (p.type == type) return &p;
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("frei0r");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    // ---- Registry ----
    int count = 0;
    QSet<QString> names;
    for (const EffectDescriptor& d : EffectRegistry::all())
        if (d.category == EffectRegistry::Frei0rCategory) {
            ++count;
            CHECK(!names.contains(d.name)); // old alias names hidden
            names << d.name;
            CHECK(d.library && d.video && d.id == d.mltService && d.id.startsWith("frei0r."));
        }
    if (!EffectRegistry::find("frei0r.flippo")) return Check::skip("frei0r plugins not installed");
    CHECK(count > 50);
    CHECK(!EffectRegistry::find("frei0r.bluescreen0r")); // own section "Green Screen"
    CHECK(EffectRegistry::find("chromakey"));
    if (EffectRegistry::find("frei0r.measure_pr0be")) CHECK(!EffectRegistry::find("frei0r.pr0be")); // Alias
    if (const EffectDescriptor* d = EffectRegistry::find("frei0r.glow"); CHECK(d)) {
        CHECK_EQ(d->name, QString("Glow"));
        const EffectParam* p = param(d, EffectParam::Double);
        if (CHECK(p)) CHECK(p->key == "0" && p->mltProperty == "0" && p->min == 0 && p->max == 1);
    }
    if (const EffectDescriptor* d = EffectRegistry::find("frei0r.three_point_balance"); CHECK(d)) {
        CHECK(param(d, EffectParam::Color) && param(d, EffectParam::Bool));
        CHECK(param(d, EffectParam::Color)->defaultValue.value<QColor>().isValid());
    }
    if (const EffectDescriptor* d = EffectRegistry::find("frei0r.colortap"); CHECK(d)) {
        const EffectParam* p = param(d, EffectParam::Choice);
        if (CHECK(p)) CHECK(p->choices.contains("sepia") && p->choices.contains("xray") && p->defaultValue == "esses");
    }

    // ---- Rendering ----
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg not found");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_media = Check::makeMedia(tmp.filePath("bars.mp4"), {"-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=2",
                                                          "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    ProjectFormat fmt;
    fmt.width = 640;
    fmt.height = 360;

    const QColor ref = left(render(one([](Clip&) {}), fmt, 5));
    CHECK(ref.red() > 150 && std::abs(ref.red() - ref.blue()) < 15); // gray bar found

    // No parameters: invert
    const QColor inv = left(render(one(fx("frei0r.invert0r")), fmt, 5));
    CHECK(std::abs(inv.red() - (255 - ref.red())) < 15);
    // disabled = unchanged
    const QColor off = left(render(one([](Clip& c) {
                                fx("frei0r.invert0r")(c);
                                c.effects[0].enabled = false;
                            }), fmt, 5));
    CHECK(std::abs(off.red() - ref.red()) < 5);
    // Checkbox: mirror on X -> the blue bar is now on the left
    const QColor same = left(render(one(fx("frei0r.flippo")), fmt, 5));
    CHECK(std::abs(same.red() - ref.red()) < 10);
    const QColor flipped = left(render(one(fx("frei0r.flippo", {{"0", true}})), fmt, 5));
    CHECK(flipped.blue() > 150 && flipped.red() < 60);
    // Choice: different tables tint differently
    const QColor sepia = left(render(one(fx("frei0r.colortap", {{"0", "sepia"}})), fmt, 5));
    const QColor xray = left(render(one(fx("frei0r.colortap", {{"0", "xray"}})), fmt, 5));
    CHECK(std::abs(sepia.red() - xray.red()) + std::abs(sepia.blue() - xray.blue()) > 30);
    CHECK(sepia.red() > sepia.blue()); // sepia is warm

    // ---- Save/load: types survive (color, checkbox, choice, number) ----
    ProjectData d;
    d.timeline = one([](Clip& c) {
        fx("frei0r.three_point_balance")(c);
        const EffectDescriptor* tpb = EffectRegistry::find("frei0r.three_point_balance");
        EffectInstance* e = EffectRegistry::instance(c, tpb->id);
        e->params[param(tpb, EffectParam::Color)->key] = QColor(10, 20, 30);
        e->params[param(tpb, EffectParam::Bool)->key] = true;
        fx("frei0r.colortap", {{"0", "xray"}})(c);
        fx("frei0r.glow", {{"0", 0.375}})(c);
        c.effects.last().enabled = false;
    });
    d.media = {MediaInfo{g_media, "bars.mp4", 50, true, false, false}};
    const QString path = tmp.filePath("p.schneidi");
    QString err;
    ProjectData loaded;
    if (CHECK(ProjectFile::save(d, path, &err)) && CHECK(ProjectFile::load(path, &loaded, &err))) {
        const Clip& a = d.timeline.video[0].clips[0];
        const Clip& b = loaded.timeline.video[0].clips[0];
        CHECK(a.effects == b.effects);
        CHECK(b.effects[0].params.value("0").typeId() == QMetaType::QColor);
    }

    // ---- Keyframes on frei0r parameters ----
    const EffectDescriptor* bright = EffectRegistry::find("frei0r.brightness");
    const AnimParam ba = bright && !bright->params.isEmpty() ? bright->params[0].anim : AnimParam::Count;
    if (CHECK(bright && ba != AnimParam::Count)) {
        CHECK(Keys::isEffectParam(ba));
        CHECK_EQ(QString(Keys::info(ba).id), QString("fx:frei0r.brightness:0"));
        AnimParam back = AnimParam::Count;
        CHECK(Keys::fromId("fx:frei0r.brightness:0", &back) && back == ba);
        CHECK(Keys::effectParam("frei0r.brightness", "0") == ba); // same pair = same value
        CHECK(Keys::effectParam("frei0r.brightness", "1") != ba);
        CHECK_EQ(Keys::label(ba), QString("Brightness: Brightness"));
        double lo = 0, hi = 0;
        Keys::range(ba, &lo, &hi);
        CHECK(lo == 0 && hi == 1);
        const EffectDescriptor* e = nullptr;
        const EffectParam* ep = nullptr;
        CHECK(EffectRegistry::paramFor(ba, &e, &ep) && e == bright && ep->key == "0");
        // Key with ':' in the key part and unknown effects still parse (project from another computer)
        AnimParam other = AnimParam::Count;
        CHECK(Keys::fromId("fx:frei0r.gibtsnicht:a:b", &other) && Keys::isEffectParam(other));
        QString eid, key;
        CHECK(Keys::effectParamOf(other, &eid, &key) && eid == "frei0r.gibtsnicht" && key == "a:b");
        CHECK(!Keys::fromId("fx:", &other) && !Keys::fromId("fx:x", &other));

        // Values: static from the instance, animated from the keys (source frames: in + t)
        Clip c;
        c.in = 10;
        c.out = 59;
        CHECK(EffectRegistry::add(c, "frei0r.brightness"));
        CHECK(Keys::staticValue(c, ba) == 0.5);
        Keys::setValue(c, ba, 0, 0.25); // not animated -> instance
        CHECK(EffectRegistry::value(c, "frei0r.brightness", "0").toDouble() == 0.25);
        Keys::setKey(c, ba, 0, 0.0);
        Keys::setKey(c, ba, 40, 1.0);
        CHECK(Keys::animated(c, ba) && c.keys[ba].first().frame == 10);
        CHECK(qAbs(Keys::valueAt(c, ba, 20) - 0.5) < 1e-9);
        Keys::removeKey(c, ba, 0);
        Keys::removeKey(c, ba, 40); // last one gone: value stays as static value
        CHECK(!Keys::animated(c, ba) && EffectRegistry::value(c, "frei0r.brightness", "0").toDouble() == 1.0);
        Keys::setKey(c, ba, 5, 0.3);
        EffectRegistry::remove(c, "frei0r.brightness"); // keys go with the effect
        CHECK(c.keys.isEmpty());
        Clip u;
        u.effects << EffectInstance{"frei0r.gibtsnicht", {{"a:b", 0.3}}, true};
        Keys::setKey(u, other, 0, 0.1);
        EffectRegistry::remove(u, "frei0r.gibtsnicht"); // also without a descriptor
        CHECK(u.keys.isEmpty() && u.effects.isEmpty());

        // Render: dark at the first key, neutral in the middle, bright at the last
        auto animated = [&](int in) {
            return one([&, in](Clip& clip) {
                clip.in = in;
                clip.out = 49;
                fx("frei0r.brightness")(clip);
                Keys::setKey(clip, ba, 0, 0.0);
                Keys::setKey(clip, ba, 40 - in, 1.0);
            });
        };
        for (int in : {0, 20}) { // in 20: keys count from the cut, not from source frame 0
            const Timeline tl = animated(in);
            const QColor dark = left(render(tl, fmt, 0));
            const QColor mid = left(render(tl, fmt, (40 - in) / 2));
            const QColor light = left(render(tl, fmt, 40 - in));
            CHECK(dark.red() < ref.red() - 60);
            CHECK(std::abs(mid.red() - ref.red()) < 25);
            CHECK(light.red() > ref.red() + 30);
        }

        // Project file: id "fx:<effect>:<key>", also for a missing plugin (stays in the file)
        ProjectData kd;
        kd.timeline = animated(0);
        Clip& kc = kd.timeline.video[0].clips[0];
        kc.effects << EffectInstance{"frei0r.gibtsnicht", {{"a:b", 0.3}}, true};
        Keys::setKey(kc, other, 3, 0.7);
        Keys::setKey(kc, other, 9, 0.2);
        kc.keys[ba][1].ease = KeyEase::EaseIn;
        const QJsonObject keysJson = ProjectFile::clipJson(kc).value("keys").toObject();
        CHECK(keysJson.contains("fx:frei0r.brightness:0") && keysJson.contains("fx:frei0r.gibtsnicht:a:b"));
        kd.media = d.media;
        const QString kpath = tmp.filePath("keys.schneidi");
        ProjectData kl;
        if (CHECK(ProjectFile::save(kd, kpath, &err)) && CHECK(ProjectFile::load(kpath, &kl, &err)))
            CHECK(kl.timeline.video[0].clips[0] == kc);
    }

    // ---- Inspector ----
    const QString dump = QString::fromLocal8Bit(qgetenv("FREI0R_DUMP"));
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(d.media[0]);
    editor.addMediaAt({g_media}, 0, 0);
    const int vid = project.timeline().video[0].clips[0].id;
    Inspector insp(&editor);
    insp.resize(380, 900);
    sel.set({vid});
    insp.show();
    app.processEvents();
    const int boxesBefore = int(insp.findChildren<QCheckBox*>().size());
    editor.addEffect({vid}, "frei0r.flippo");
    editor.addEffect({vid}, "frei0r.colortap");
    app.processEvents();
    // Sections created only now: two checkboxes (X/Y axis), one choice with the tables
    QVector<QCheckBox*> boxes;
    for (QCheckBox* b : insp.findChildren<QCheckBox*>())
        if (b->isVisibleTo(&insp) && b->text().isEmpty()) boxes << b;
    CHECK_EQ(int(insp.findChildren<QCheckBox*>().size()), boxesBefore + 2);
    QComboBox* tables = nullptr;
    for (QComboBox* c : insp.findChildren<QComboBox*>())
        if (c->findText("sepia") >= 0) tables = c;
    if (CHECK(boxes.size() == 2 && tables)) {
        CHECK(!boxes[0]->isChecked() && tables->currentText() == "esses");
        const int steps = project.undoStack()->count();
        boxes[0]->click();
        auto clip = [&] { return project.timeline().video[0].clips[0]; };
        CHECK(EffectRegistry::value(clip(), "frei0r.flippo", "0").toBool());
        CHECK_EQ(project.undoStack()->count(), steps + 1);
        tables->activated(tables->findText("xray"));
        CHECK_EQ(EffectRegistry::value(clip(), "frei0r.colortap", "0").toString(), QString("xray"));
        project.undoStack()->undo();
        project.undoStack()->undo();
        app.processEvents();
        CHECK(!boxes[0]->isChecked() && tables->currentText() == "esses"); // display follows the undo
    }
    editor.addEffect({vid}, "frei0r.three_point_balance");
    app.processEvents();
    if (!dump.isEmpty()) insp.grab().save(QDir(dump).filePath("inspector.png"));
    // Removing hides the section
    editor.removeEffect({vid}, "frei0r.colortap");
    app.processEvents();
    CHECK(tables && !tables->isVisibleTo(&insp));

    // Keyframe diamond of a frei0r number parameter: one undo step, key at the playhead
    if (ba != AnimParam::Count) {
        auto diamonds = [&] {
            QVector<QToolButton*> out;
            for (QToolButton* b : insp.findChildren<QToolButton*>())
                if (b->toolTip().startsWith(T("Keyframe setzen/entfernen")) && b->isVisibleTo(&insp)) out << b;
            return out;
        };
        const auto before = diamonds();
        editor.addEffect({vid}, "frei0r.brightness");
        app.processEvents();
        const auto after = diamonds();
        QToolButton* diamond = nullptr;
        for (QToolButton* b : after)
            if (!before.contains(b)) diamond = b;
        if (CHECK(diamond && after.size() == before.size() + 1)) {
            const int steps = project.undoStack()->count();
            diamond->click();
            auto clip = [&] { return project.timeline().video[0].clips[0]; };
            CHECK(Keys::animated(clip(), ba) && Keys::keyAt(clip(), ba, 0));
            CHECK_EQ(project.undoStack()->count(), steps + 1);
            app.processEvents();
            CHECK_EQ(diamond->text(), QString("◆")); // key at the playhead
            project.undoStack()->undo();
            app.processEvents();
            CHECK(!Keys::animated(clip(), ba) && diamond->text() == QString("◇"));
        }
    }

    // ---- Missing plugin ----
    {
        ProjectData md = d;
        md.timeline.video[0].clips[0].effects << EffectInstance{"frei0r.gibtsnicht", {{"0", 0.5}}, true};
        Sequence seq;
        seq.timeline = one(fx("frei0r.invert0r"));
        seq.timeline.video[0].clips[0].effects << EffectInstance{"frei0r.auchnicht", {}, true}
                                               << EffectInstance{"frei0r.gibtsnicht", {}, true};
        md.sequences << seq;
        CHECK_EQ(ProjectFile::missingEffects(md), (QStringList{"frei0r.auchnicht", "frei0r.gibtsnicht"}));
        CHECK(ProjectFile::missingEffects(d).isEmpty());
        // Rendering skips it, the rest still works
        const QColor still = left(render(one([](Clip& c) {
                                      c.effects << EffectInstance{"frei0r.gibtsnicht", {}, true};
                                      fx("frei0r.invert0r")(c);
                                  }), fmt, 5));
        CHECK(std::abs(still.red() - inv.red()) < 15);
    }
    {
        auto clip = [&] { return project.timeline().video[0].clips[0]; };
        auto warning = [&]() -> QLabel* {
            for (QLabel* l : insp.findChildren<QLabel*>("MissingEffect"))
                if (l->isVisibleTo(&insp)) return l;
            return nullptr;
        };
        CHECK(!warning());
        editor.modifyClips({vid}, "unknown", [](Clip& c) {
            c.effects << EffectInstance{"frei0r.gibtsnicht", {{"0", 0.5}}, true};
        });
        app.processEvents();
        CHECK(warning());
        QToolButton* title = nullptr;
        for (QToolButton* b : insp.findChildren<QToolButton*>())
            if (b->text() == T("%1 (fehlt)").arg("frei0r.gibtsnicht") && b->isVisibleTo(&insp)) title = b;
        CHECK(title);
        if (!dump.isEmpty()) insp.grab().save(QDir(dump).filePath("missing.png"));
        const int steps = project.undoStack()->count();
        editor.removeEffect({vid}, "frei0r.gibtsnicht"); // like the trash button
        app.processEvents();
        CHECK(!EffectRegistry::has(clip(), "frei0r.gibtsnicht") && !warning());
        CHECK_EQ(project.undoStack()->count(), steps + 1);
        project.undoStack()->undo();
        app.processEvents();
        CHECK(EffectRegistry::has(clip(), "frei0r.gibtsnicht") && warning());
    }

    // ---- Effects Library: Open FX → frei0r ----
    EffectsLibrary lib;
    lib.resize(520, 420);
    auto* tree = lib.findChild<QTreeWidget*>();
    auto* list = lib.findChild<QListWidget*>();
    const auto hits = tree->findItems("frei0r", Qt::MatchExactly | Qt::MatchRecursive);
    if (CHECK(hits.size() == 1)) {
        tree->setCurrentItem(hits.first());
        CHECK_EQ(list->count(), count);
        lib.show();
        app.processEvents();
        if (!dump.isEmpty()) lib.grab().save(QDir(dump).filePath("library.png"));
    }
    return Check::result();
}
