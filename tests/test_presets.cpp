// Test presets from Kdenlive and Shotcut (core/Presets): custom effect with <parameter factor>, effect group with
// keyframes (animation strings in frames, clock and SMPTE time, interpolation types, parentIn) and parameter names, Shotcut filter set (MLT XML) and filter preset (folder name = filter), colors and
// choices, unknown filters skipped, LUT from avfilter.lut3d, broken files; applying via the Editor (one undo step,
// existing effect takes the values) and the Effects Library category "Presets".
// Picture of the Library: PRESETS_DUMP=<png> build-tests/tests/test_presets
#include "check.h"

#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
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
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>

namespace {

double num(const EffectInstance& e, const QString& key) { return e.params.value(key).toDouble(); }

AnimParam animOf(const QString& effect, const QString& key)
{
    if (const EffectDescriptor* d = EffectRegistry::find(effect))
        for (const EffectParam& p : d->params)
            if (p.key == key) return p.anim;
    return AnimParam::Count;
}

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
        CHECK(!m.keyframes); // Gamma is a number: keyframes taken over
        const AnimParam gamma = animOf("frei0r.levels", "3");
        if (CHECK(m.keys.contains(gamma) && m.keys.size() == 1)) {
            const QVector<Presets::AnimKey>& k = m.keys[gamma];
            // parentIn="1": the first key lies on the first frame of the clip
            CHECK(k.size() == 3 && k[0].frame == 0 && k[1].frame == 9 && k[2].frame == 11);
            CHECK(qAbs(k[1].value - 0.2455) < 1e-9 && qAbs(k[2].value - 0.485) < 1e-9);
        }
    }

    // ---- Animation strings ----
    {
        QVector<Presets::RawKey> k;
        CHECK(Presets::parseAnimation("1=0;10=15", 25, &k) && k.size() == 2 && k[1].frame == 10 && k[1].value == "15");
        CHECK(Presets::parseAnimation("00:00:00.000=0.2;00:00:01.000=0.9", 25, &k) && k[1].frame == 25);
        CHECK(Presets::parseAnimation("00:00:01.000=0.9", 50, &k) && k[0].frame == 50);
        CHECK(Presets::parseAnimation("00:01.500=1", 30, &k) && k[0].frame == 45);       // mm:ss.zzz
        CHECK(Presets::parseAnimation("00:00:01:05=1", 25, &k) && k[0].frame == 30);     // SMPTE
        CHECK(Presets::parseAnimation("0=1;-1=0", 25, &k) && k[1].frame == -1);           // from the end
        CHECK(Presets::parseAnimation("0~=1;10|=2;20c=3;30-=4;40=5", 25, &k) && k.size() == 5);
        CHECK(k[0].type == '~' && k[1].type == '|' && k[2].type == 'c' && k[3].type == '-' && k[4].type.isNull());
        CHECK(Presets::parseAnimation("0=0 0 1920 1080 1", 25, &k) && k[0].value == "0 0 1920 1080 1");
        for (const char* plain : {"0.5", "sepia", "#ff00ff00", "", "a=b", "1=2;x=3"})
            CHECK(!Presets::parseAnimation(plain, 25, &k));

        using AK = Presets::AnimKey;
        // discrete: hold until the frame before the next key
        KeyTrack t = Presets::toKeyTrack({AK{0, 1, '|'}, AK{10, 2, {}}}, 50);
        if (CHECK(t.size() == 3)) CHECK(t[1].frame == 9 && t[1].value == 1 && t[2].value == 2);
        // eases: in = slow start at the first key, out = slow end at the second, in/out both
        t = Presets::toKeyTrack({AK{0, 0, 'a'}, AK{10, 1, 'e'}, AK{20, 0, 'i'}, AK{30, 1, {}}}, 50);
        if (CHECK(t.size() == 4)) {
            CHECK(t[0].ease == KeyEase::EaseOut);
            CHECK(t[1].ease == KeyEase::Linear);
            CHECK(t[2].ease == KeyEase::EaseInOut); // slow end of 'e' + slow start of 'i'
            CHECK(t[3].ease == KeyEase::EaseIn);
        }
        // smooth: Bezier with soft handles; negative frames from the end, sorted
        t = Presets::toKeyTrack({AK{-1, 3, {}}, AK{0, 0, '~'}, AK{20, 2, '~'}}, 50);
        if (CHECK(t.size() == 3)) {
            CHECK(t[0].frame == 0 && t[1].frame == 20 && t[2].frame == 49);
            CHECK(t[0].ease == KeyEase::Bezier && t[1].ease == KeyEase::Bezier && t[2].ease == KeyEase::Bezier);
            CHECK(t[1].outDv > 0); // rising through the middle key
        }
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
        CHECK(!a.keyframes);
        const AnimParam glow = animOf("frei0r.glow", "0");
        if (CHECK(a.keys.contains(glow))) CHECK(a.keys[glow].size() == 2 && a.keys[glow][1].frame == 25);
        const Presets::Mapped a50 = Presets::map(Presets::parse("0=00:00:00.000=0.2;00:00:01.000=0.9\n", "a", "glow"), 50);
        CHECK(a50.keys.value(glow).value(1).frame == 50);
        // Animated checkbox: cannot be animated -> first value, flagged
        const Presets::Mapped b = Presets::map(Presets::parse("0=0=1;10=0\n", "b", "flippo"));
        if (const EffectInstance* e = effect(b, "frei0r.flippo"); CHECK(e)) CHECK(e->params.value("0").toBool());
        CHECK(b.keyframes && b.keys.isEmpty());
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
        c.in = 5; // keys count in source frames
        c.out = 54;
        tl.video[0].clips << c;
    });
    auto clip = [&] { return project.timeline().video[0].clips[0]; };
    const int steps = project.undoStack()->count();
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("Kdenlive/shut_off.xml"));
    CHECK_EQ(project.undoStack()->count(), steps + 1);
    CHECK(EffectRegistry::has(clip(), "frei0r.levels") && EffectRegistry::has(clip(), "frei0r.colortap"));
    const AnimParam gamma = animOf("frei0r.levels", "3");
    if (CHECK(Keys::animated(clip(), gamma))) { // keyframes in clip frames
        CHECK(qAbs(Keys::valueAt(clip(), gamma, 0) - 0.25) < 1e-9);
        CHECK(qAbs(Keys::valueAt(clip(), gamma, 11) - 0.485) < 1e-9);
        CHECK_EQ(Keys::keyTimes(clip(), {gamma}), (QVector<int>{0, 9, 11}));
        CHECK_EQ(clip().keys.value(gamma).value(0).frame, 5);
    }
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
    // An animated preset, then a static one: the static values replace the animation
    {
        QTemporaryDir other;
        const QString puls = QDir(other.path()).filePath("glow/Puls");
        CHECK(write(puls, "0=0=0.1;-1=0.6\n"));
        editor.addEffect({1}, Presets::Prefix + puls);
        const AnimParam glow = animOf("frei0r.glow", "0");
        CHECK_EQ(Keys::keyTimes(clip(), {glow}), (QVector<int>{0, 49})); // -1 = last frame of the clip
        editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("glow/Hell"));
        CHECK(!Keys::animated(clip(), glow));
    }

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
