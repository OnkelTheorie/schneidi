// Test presets from Kdenlive and Shotcut (core/Presets): custom effect with <parameter factor>, effect group with
// keyframes and parameter names, Shotcut filter set (MLT XML) and filter preset (folder name = filter), colors and
// choices, unknown filters skipped, LUT from avfilter.lut3d, broken files; applying via the Editor (one undo step,
// existing effect takes the values) and the Effects Library category "Presets".
// Picture of the Library: PRESETS_DUMP=<png> build-tests/tests/test_presets
#include "check.h"

#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "ui/EffectsLibrary.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QTreeWidget>
#include <QUndoStack>
#include <clocale>

namespace {

double num(const EffectInstance& e, const QString& key) { return e.params.value(key).toDouble(); }

const EffectInstance* effect(const Presets::Mapped& m, const QString& id)
{
    for (const EffectInstance& e : m.effects)
        if (e.effectId == id) return &e;
    return nullptr;
}

bool write(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

const QByteArray kKdenliveEffect = R"(<?xml version="1.0"?>
<!DOCTYPE kpartgui>
<effect tag="frei0r.glow" id="frei0r.glow" type="customVideo">
    <name>Starker Glow</name>
    <description>Viel Glow</description>
    <parameter type="animated" name="Blur" default="10" value="700" min="0" max="1000" factor="1000"><name>Blur</name></parameter>
</effect>
)";

const QByteArray kKdenliveGroup = R"(<?xml version="1.0"?>
<effectgroup id="Shut-off" parentIn="1">
    <effect id="frei0r.levels">
        <property name="kdenlive:collapsed">1</property>
        <property name="Gamma">1=0.25;10=0.2455;12=0.485</property>
        <property name="Input white level">0.8</property>
    </effect>
    <effect id="boxblur"><property name="blur">1=0;10=15</property></effect>
    <effect id="frei0r.colortap"><property name="Table">sepia</property></effect>
    <description>CRT monitor shut off effect</description>
</effectgroup>
)";

const QByteArray kShotcutSet = R"(<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" version="7.21.0" root="" parent="producer0">
  <producer id="producer0">
    <property name="mlt_service">color</property>
    <filter id="filter0">
      <property name="filter">shape</property>
      <property name="mlt_service">mask_start</property>
    </filter>
    <filter id="filter1">
      <property name="version">0.1</property>
      <property name="mlt_service">frei0r.glow</property>
      <property name="0">0.5</property>
    </filter>
    <filter id="filter2">
      <property name="mlt_service">frei0r.three_point_balance</property>
      <property name="0">0xff000080</property>
      <property name="3">1</property>
    </filter>
  </producer>
</mlt>
)";

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("presets");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    if (!EffectRegistry::find("frei0r.glow") || !EffectRegistry::find("frei0r.levels"))
        return Check::skip("frei0r plugins not installed");

    // ---- Kdenlive custom effect: <parameter value> in shown units (factor) ----
    {
        const Presets::Preset p = Presets::parse(kKdenliveEffect, "glow.xml");
        CHECK(p.error.isEmpty());
        CHECK_EQ(p.name, QString("Starker Glow"));
        CHECK_EQ(p.source, QString("Kdenlive"));
        CHECK_EQ(p.description, QString("Viel Glow"));
        const Presets::Mapped m = Presets::map(p);
        if (const EffectInstance* e = effect(m, "frei0r.glow"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.7) < 1e-9);
        CHECK(m.skipped.isEmpty() && !m.keyframes);
    }

    // ---- Kdenlive effect group: names instead of indexes, keyframes -> first value, unknown filter skipped ----
    {
        const Presets::Preset p = Presets::parse(kKdenliveGroup, "shut_off.xml");
        CHECK(p.error.isEmpty());
        CHECK_EQ(p.name, QString("Shut-off"));
        CHECK_EQ(int(p.filters.size()), 3);
        const Presets::Mapped m = Presets::map(p);
        if (const EffectInstance* e = effect(m, "frei0r.levels"); CHECK(e)) {
            CHECK(qAbs(num(*e, "3") - 0.25) < 1e-9); // Gamma
            CHECK(qAbs(num(*e, "2") - 0.8) < 1e-9);  // Input white level
        }
        if (const EffectInstance* e = effect(m, "frei0r.colortap"); CHECK(e))
            CHECK_EQ(e->params.value("0").toString(), QString("sepia"));
        CHECK_EQ(m.skipped, QStringList{"boxblur"});
        CHECK(m.keyframes);
    }

    // ---- Shotcut filter set: indexes, MLT colors, mask filters skipped ----
    {
        const Presets::Preset p = Presets::parse(kShotcutSet, "Glow Intensity");
        CHECK(p.error.isEmpty());
        CHECK_EQ(p.name, QString("Glow Intensity"));
        CHECK_EQ(p.source, QString("Shotcut"));
        const Presets::Mapped m = Presets::map(p);
        if (const EffectInstance* e = effect(m, "frei0r.glow"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.5) < 1e-9);
        if (const EffectInstance* e = effect(m, "frei0r.three_point_balance"); CHECK(e)) {
            const QColor c = e->params.value("0").value<QColor>();
            CHECK(c.red() == 255 && c.green() == 0 && c.blue() == 0 && c.alpha() == 0x80);
            CHECK(e->params.value("3").toBool());
        }
        CHECK_EQ(m.skipped, QStringList{"mask_start"});
    }

    // ---- Shotcut filter preset: "property=value", filter = folder name (QML objectName) ----
    {
        const Presets::Preset p = Presets::parse("0=0.3\n1=1\nshotcut:animIn=00:00:00.000\n", "Weich", "blur_exponential");
        CHECK(p.error.isEmpty());
        const Presets::Mapped m = Presets::map(p);
        if (const EffectInstance* e = effect(m, "frei0r.IIRblur"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.3) < 1e-9);
        CHECK_EQ(Presets::shotcutService("glow"), QString("frei0r.glow"));
        CHECK_EQ(Presets::shotcutService("frei0r.glow"), QString("frei0r.glow"));
        CHECK(Presets::shotcutService("gibtsnicht").isEmpty());
        CHECK(!Presets::parse("0=1\n", "x", "gibtsnicht").error.isEmpty());
        // Animated value in Shotcut's time format
        const Presets::Mapped a = Presets::map(Presets::parse("0=00:00:00.000=0.2;00:00:01.000=0.9\n", "a", "glow"));
        if (const EffectInstance* e = effect(a, "frei0r.glow"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.2) < 1e-9);
        CHECK(a.keyframes);
    }

    // ---- Broken / unknown files ----
    CHECK(!Presets::parse("", "leer").error.isEmpty());
    CHECK(!Presets::parse("<html><body/></html>", "x.xml").error.isEmpty());
    CHECK(!Presets::parse("<effect id=\"x\"", "kaputt.xml").error.isEmpty());
    CHECK(!Presets::parse("nur text\n", "x").error.isEmpty());
    CHECK(Presets::map(Presets::parse("<effect tag=\"boxblur\"/>", "b.xml")).empty());

    // ---- LUT: avfilter.lut3d -> color correction; missing file found by name in the own LUTs folder ----
    EffectFolders::ensure();
    const QString cube = QDir(EffectFolders::lutDir()).filePath("preset-test.cube");
    CHECK(write(cube, "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"));
    {
        const QByteArray xml = R"(<effect tag="avfilter.lut3d"><property name="av.file">C:\Users\x\luts\preset-test.cube</property></effect>)";
        const Presets::Mapped m = Presets::map(Presets::parse(xml, "lut.xml"));
        CHECK_EQ(m.lut, cube);
        CHECK(m.effects.isEmpty() && !m.empty());
    }

    // ---- Editor: one undo step, existing effect takes the preset's values ----
    const QString dir = EffectFolders::presetDir();
    CHECK(write(QDir(dir).filePath("Kdenlive/glow.xml"), kKdenliveEffect));
    CHECK(write(QDir(dir).filePath("Kdenlive/shut_off.xml"), kKdenliveGroup));
    CHECK(write(QDir(dir).filePath("glow/Hell"), "0=0.9\n"));
    CHECK(write(QDir(dir).filePath("kaputt.xml"), "<effect"));
    CHECK(write(QDir(dir).filePath("Liesmich.txt"), "kein Preset"));
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.edit("setup", [](Timeline& tl) {
        tl.video.resize(1);
        Clip c;
        c.id = 1;
        c.mediaPath = "clip.mp4";
        c.out = 49;
        tl.video[0].clips << c;
    });
    auto clip = [&] { return project.timeline().video[0].clips[0]; };
    const int steps = project.undoStack()->count();
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("Kdenlive/shut_off.xml"));
    CHECK_EQ(project.undoStack()->count(), steps + 1);
    CHECK(EffectRegistry::has(clip(), "frei0r.levels") && EffectRegistry::has(clip(), "frei0r.colortap"));
    CHECK_EQ(int(clip().effects.size()), 2);
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("Kdenlive/glow.xml"));
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("glow/Hell"));
    CHECK_EQ(int(clip().effects.size()), 3); // glow only once, with the later values
    CHECK(qAbs(EffectRegistry::value(clip(), "frei0r.glow", "0").toDouble() - 0.9) < 1e-9);
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("kaputt.xml")); // nothing happens
    CHECK_EQ(project.undoStack()->count(), steps + 3);
    project.undoStack()->undo();
    CHECK(qAbs(EffectRegistry::value(clip(), "frei0r.glow", "0").toDouble() - 0.7) < 1e-9);
    project.undoStack()->undo();
    project.undoStack()->undo();
    CHECK(clip().effects.isEmpty());

    // ---- Effects Library: Presets, subfolders as categories, broken file shown but not usable ----
    EffectsLibrary lib;
    auto* tree = lib.findChild<QTreeWidget*>();
    auto* list = lib.findChild<QListWidget*>();
    const auto hits = tree->findItems("Presets", Qt::MatchExactly | Qt::MatchRecursive);
    if (CHECK(hits.size() == 1)) {
        CHECK_EQ(hits.first()->childCount(), 2); // glow, Kdenlive
        tree->setCurrentItem(hits.first());
        QStringList names, usable;
        for (int i = 0; i < list->count(); ++i) {
            names << list->item(i)->text();
            if (!list->item(i)->data(Qt::UserRole).toString().isEmpty()) usable << list->item(i)->text();
        }
        if (const QString dump = QString::fromLocal8Bit(qgetenv("PRESETS_DUMP")); !dump.isEmpty()) {
            lib.resize(520, 300);
            lib.show();
            app.processEvents();
            lib.grab().save(dump);
        }
        names.sort();
        usable.sort();
        CHECK_EQ(names, (QStringList{"Hell", "Shut-off", "Starker Glow", "kaputt"}));
        CHECK_EQ(usable, (QStringList{"Hell", "Shut-off", "Starker Glow"}));
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->text() == "Shut-off") CHECK(list->item(i)->toolTip().contains("boxblur"));
        const auto sub = tree->findItems("Kdenlive", Qt::MatchExactly | Qt::MatchRecursive);
        if (CHECK(sub.size() == 1)) {
            tree->setCurrentItem(sub.first());
            CHECK_EQ(list->count(), 2);
        }
    }
    QDir(EffectFolders::root()).removeRecursively();
    return Check::result();
}
