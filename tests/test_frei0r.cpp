// Test frei0r plugins as effects: registry from the MLT metadata (number, checkbox, color, choice; old alias names
// only once), rendering with parameters (pixel values on color bars), saving/loading parameters with their type,
// Inspector (sections appear with the first clip that has the effect, checkbox/choice with undo) and Effects Library.
// Pictures of Inspector and Library: FREI0R_DUMP=<folder> build-tests/tests/test_frei0r
#include "check.h"

#include "core/Editor.h"
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
#include <QListWidget>
#include <QSet>
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
