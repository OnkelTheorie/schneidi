// Test presets from Kdenlive and Shotcut (core/Presets): custom effect as Kdenlive 24.12 writes it (value = MLT value) and from before 19.04 (factor), effect group with
// keyframes (animation strings in frames, clock and SMPTE time, interpolation types, parentIn) and parameter names, Shotcut filter set (MLT XML) and filter preset (folder name = filter), colors and
// choices, MLT filters rebuilt with own effects (brightness/eq/greyscale -> color, blurs -> blur, lift_gamma_gain ->
// grade, merged into existing values), unknown filters skipped, LUT from avfilter.lut3d, broken files; applying via the Editor (one undo step,
// existing effect takes the values) and the Effects Library category "Presets". Own presets: clip effects -> Kdenlive
// <effectgroup> / custom effect in the shape Kdenlive accepts (its parameter names) -> read back (values, disabled
// effects, keyframes incl. eases and trimmed keys), file name, Library.
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
#include <QFileInfo>
#include <QFile>
#include <QListWidget>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QXmlStreamReader>
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

// As Kdenlive 24.12 writes it ("Save effect": frei0r_glow.xml with id = name and value = MLT value; factor only
// scales the slider)
const QByteArray kKdenliveEffect = R"(<!DOCTYPE kpartgui>
<effect tag="frei0r.glow" id="Starker Glow" type="customVideo">
    <name>Starker Glow</name>
    <description>Viel Glow</description>
    <author>Richard Spindler</author>
    <parameter type="animated" name="Blur" default="0.01" min="0" max="1000" factor="1000" value="0.7">
        <name>Blur</name>
    </parameter>
</effect>
)";

// Custom effect from Kdenlive before 19.04 (kdenlive_info): values were stored in shown units (MLT value * factor)
const QByteArray kKdenliveLegacy = R"(<effect tag="frei0r.glow" id="Alt" kdenlive_info="" type="custom">
    <name>Alt</name>
    <parameter type="constant" name="Blur" default="10" value="400" min="0" max="1000" factor="1000"/>
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
    <effect id="qtblend"><property name="rect">1=0 0 1920 1080 1.000000;10=-1527 -861 4974 2798 1.000000</property></effect>
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

// Kdenlive's rules for custom effect files (EffectsRepository::parseCustomAssetFile, 24.12): an <effectgroup> needs
// more than one <effect>, each id an effect Kdenlive knows (here: an MLT filter); a single effect needs tag= with an
// MLT filter and <parameter> definitions. Elements other than <effect> are ignored.
bool kdenliveAccepts(const QByteArray& xml)
{
    QXmlStreamReader x(xml);
    if (!x.readNextStartElement()) return false;
    const bool group = x.name() == u"effectgroup";
    if (!group) return x.name() == u"effect" && x.attributes().value("tag").startsWith(u"frei0r.") &&
                         xml.contains("<parameter");
    int effects = 0;
    while (!x.atEnd()) {
        x.readNext();
        if (!x.isStartElement() || x.name() != u"effect") continue;
        ++effects;
        const QString id = x.attributes().value("id").toString();
        if (std::none_of(EffectRegistry::all().begin(), EffectRegistry::all().end(),
                         [&](const EffectDescriptor& d) { return d.mltService == id; }))
            return false;
    }
    return !x.hasError() && effects > 1;
}

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

    // ---- Kdenlive custom effect: <parameter value> is the MLT value (factor only for files before 19.04) ----
    {
        const Presets::Preset p = Presets::parse(kKdenliveEffect, "glow.xml");
        CHECK(p.error.isEmpty());
        CHECK_EQ(p.name, QString("Starker Glow"));
        CHECK_EQ(p.source, QString("Kdenlive"));
        CHECK_EQ(p.description, QString("Viel Glow"));
        const Presets::Mapped m = Presets::map(p);
        if (const EffectInstance* e = effect(m, "frei0r.glow"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.7) < 1e-9);
        CHECK(m.skipped.isEmpty() && !m.keyframes);
        const Presets::Mapped legacy = Presets::map(Presets::parse(kKdenliveLegacy, "alt.xml"));
        if (const EffectInstance* e = effect(legacy, "frei0r.glow"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.4) < 1e-9);
        // Without value: default, also an MLT value
        const Presets::Mapped def = Presets::map(Presets::parse(
            R"(<effect tag="frei0r.glow" id="d"><parameter type="animated" name="Blur" default="0.01" factor="1000"/></effect>)",
            "d.xml"));
        if (const EffectInstance* e = effect(def, "frei0r.glow"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.01) < 1e-9);
    }

    // ---- Old frei0r parameter names (MLT param_name_map, still used by Kdenlive's lenscorrection/pixeliz0r) ----
    if (EffectRegistry::find("frei0r.lenscorrection")) {
        const Presets::Mapped m = Presets::map(Presets::parse(R"(<effectgroup id="g" parentIn="0">
            <effect id="frei0r.lenscorrection"><property name="xcenter">0.3</property>
                <property name="brightness">0=0.1;10=0.6</property></effect>
            <effect id="frei0r.glow"><property name="Blur">0.2</property></effect>
        </effectgroup>)", "g.xml"));
        if (const EffectInstance* e = effect(m, "frei0r.lenscorrection"); CHECK(e)) CHECK(qAbs(num(*e, "0") - 0.3) < 1e-9);
        CHECK(m.keys.value(animOf("frei0r.lenscorrection", "4")).size() == 2);
    }

    // ---- Kdenlive effect group: names instead of indexes, keyframes -> first value, unknown filter skipped ----
    {
        const Presets::Preset p = Presets::parse(kKdenliveGroup, "shut_off.xml");
        CHECK(p.error.isEmpty());
        CHECK_EQ(p.name, QString("Shut-off"));
        CHECK_EQ(int(p.filters.size()), 4);
        const Presets::Mapped m = Presets::map(p);
        if (const EffectInstance* e = effect(m, "frei0r.levels"); CHECK(e)) {
            CHECK(qAbs(num(*e, "3") - 0.25) < 1e-9); // Gamma
            CHECK(qAbs(num(*e, "2") - 0.8) < 1e-9);  // Input white level
        }
        if (const EffectInstance* e = effect(m, "frei0r.colortap"); CHECK(e))
            CHECK_EQ(e->params.value("0").toString(), QString("sepia"));
        CHECK_EQ(m.skipped, QStringList{"qtblend"});
        CHECK(!m.keyframes); // Gamma is a number: keyframes taken over
        const AnimParam gamma = animOf("frei0r.levels", "3");
        CHECK(m.keys.contains(AnimParam::FxBlur)); // boxblur -> Gaussian blur
        if (CHECK(m.keys.contains(gamma) && m.keys.size() == 2)) {
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

    // ---- MLT filters rebuilt with schneidi's effects ----
    {
        auto close = [](double a, double b) { return qAbs(a - b) < 1e-6; };
        // brightness (out = in * level) -> contrast = brightness = 100(level - 1); keys on both
        Presets::Mapped m = Presets::map(Presets::parse(
            R"(<effect tag="brightness" id="b"><parameter type="animated" name="level" default="1" value="0=1.2;20=0.5" factor="100"/></effect>)",
            "b.xml"));
        if (const EffectInstance* e = effect(m, "color"); CHECK(e)) {
            CHECK(close(num(*e, "brightness"), 20) && close(num(*e, "contrast"), 20));
            CHECK(close(num(*e, "saturation"), 0));
        }
        if (CHECK(m.keys.contains(AnimParam::FxBrightness) && m.keys.contains(AnimParam::FxContrast))) {
            CHECK(m.keys[AnimParam::FxContrast].size() == 2 && m.keys[AnimParam::FxContrast][1].frame == 20);
            CHECK(close(m.keys[AnimParam::FxBrightness][1].value, -50));
        }
        CHECK(m.skipped.isEmpty() && m.touched.value("color") == (QSet<QString>{"brightness", "contrast"}));

        // Kdenlive group: avfilter.eq + greyscale make one color correction; blurs; lift/gamma/gain -> grade
        m = Presets::map(Presets::parse(R"(<effectgroup id="Look" parentIn="0">
            <effect id="avfilter.eq"><property name="av.contrast">1.3</property><property name="av.brightness">0.1</property>
                <property name="av.saturation">0.5</property><property name="av.gamma">1</property></effect>
            <effect id="greyscale"/>
            <effect id="box_blur"><property name="hradius">5</property><property name="vradius">5</property></effect>
            <effect id="lift_gamma_gain"><property name="lift_b">0.5</property><property name="gamma_g">4</property>
                <property name="gain_r">1.5</property></effect>
        </effectgroup>)", "look.xml"));
        CHECK(m.skipped.isEmpty());
        if (const EffectInstance* e = effect(m, "color"); CHECK(e)) {
            CHECK(close(num(*e, "brightness"), 20) && close(num(*e, "contrast"), 30));
            CHECK(close(num(*e, "saturation"), -100)); // greyscale after eq
        }
        if (const EffectInstance* e = effect(m, "blur"); CHECK(e))
            CHECK(close(num(*e, "strength"), std::sqrt(10.0) * 2000 / 1080)); // box radius 5 -> sigma sqrt(10)
        if (const EffectInstance* e = effect(m, "grade"); CHECK(e)) {
            CHECK(close(num(*e, "gainR"), 1.5) && close(num(*e, "gammaG"), 1) && close(num(*e, "liftB"), std::pow(0.5, 2.2)));
            CHECK(close(num(*e, "gainG"), 1) && close(num(*e, "gammaR"), 0) && close(num(*e, "liftR"), 0));
        }
        // other blurs: gblur sigma, MLT boxblur radius = blur * hori (keys from blur), avfilter.boxblur passes
        m = Presets::map(Presets::parse(R"(<effectgroup id="B" parentIn="10">
            <effect id="boxblur"><property name="blur">10=1;30=2</property><property name="hori">3</property></effect>
            <effect id="avfilter.gblur"><property name="av.sigma">10.8</property></effect>
        </effectgroup>)", "b.xml"));
        if (const EffectInstance* e = effect(m, "blur"); CHECK(e)) CHECK(close(num(*e, "strength"), 20)); // gblur later
        CHECK(!m.keys.contains(AnimParam::FxBlur)); // static gblur replaced the boxblur animation
        m = Presets::map(Presets::parse(R"(<effectgroup id="B" parentIn="10">
            <effect id="boxblur"><property name="blur">10=1;30=2</property><property name="hori">3</property></effect>
            <effect id="frei0r.glow"/></effectgroup>)", "b.xml"));
        if (CHECK(m.keys.contains(AnimParam::FxBlur))) {
            const QVector<Presets::AnimKey>& k = m.keys[AnimParam::FxBlur];
            CHECK(k.size() == 2 && k[0].frame == 0 && k[1].frame == 20); // parentIn subtracted
            CHECK(close(k[1].value, std::sqrt(6.0 * 7 / 3) * 2000 / 1080)); // radius 2 * 3
        }
        const Presets::Mapped bb = Presets::map(Presets::parse(
            R"(<effect tag="avfilter.boxblur"><property name="av.lr">4</property><property name="av.lp">0</property></effect>)", "x.xml"));
        if (const EffectInstance* e = effect(bb, "blur"); CHECK(e)) CHECK(close(num(*e, "strength"), 0)); // power 0

        // Shotcut presets of these filters: folder = service / objectName
        CHECK_EQ(Presets::shotcutService("brightness"), QString("brightness"));
        CHECK_EQ(Presets::shotcutService("contrast"), QString("lift_gamma_gain"));
        CHECK_EQ(Presets::shotcutService("blur_gaussian_av"), QString("avfilter.gblur"));
        m = Presets::map(Presets::parse("level=0.8\nalpha=1\n", "Dunkler", "brightness"));
        if (const EffectInstance* e = effect(m, "color"); CHECK(e)) CHECK(close(num(*e, "brightness"), -20));

        // Applying keeps the clip's other values of a rebuilt effect
        Clip c;
        c.out = 49;
        CHECK(EffectRegistry::add(c, "color"));
        EffectRegistry::instance(c, "color")->params["temperature"] = 15.0;
        Keys::setKey(c, AnimParam::FxBrightness, 0, 10);
        Keys::setKey(c, AnimParam::FxBrightness, 40, 30);
        Presets::apply(c, Presets::map(Presets::parse("<effect tag=\"greyscale\"/>", "g.xml")));
        CHECK_EQ(int(c.effects.size()), 1);
        CHECK(close(EffectRegistry::value(c, "color", "saturation").toDouble(), -100));
        CHECK(close(EffectRegistry::value(c, "color", "temperature").toDouble(), 15));
        CHECK(Keys::animated(c, AnimParam::FxBrightness));
    }

    // ---- Broken / unknown files ----
    CHECK(!Presets::parse("", "leer").error.isEmpty());
    CHECK(!Presets::parse("<html><body/></html>", "x.xml").error.isEmpty());
    CHECK(!Presets::parse("<effect id=\"x\"", "kaputt.xml").error.isEmpty());
    CHECK(!Presets::parse("nur text\n", "x").error.isEmpty());
    CHECK(Presets::map(Presets::parse("<effect tag=\"qtblend\"/>", "b.xml")).empty());

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
    CHECK(EffectRegistry::has(clip(), "blur"));
    const AnimParam gamma = animOf("frei0r.levels", "3");
    if (CHECK(Keys::animated(clip(), gamma))) { // keyframes in clip frames
        CHECK(qAbs(Keys::valueAt(clip(), gamma, 0) - 0.25) < 1e-9);
        CHECK(qAbs(Keys::valueAt(clip(), gamma, 11) - 0.485) < 1e-9);
        CHECK_EQ(Keys::keyTimes(clip(), {gamma}), (QVector<int>{0, 9, 11}));
        CHECK_EQ(clip().keys.value(gamma).value(0).frame, 5);
    }
    CHECK_EQ(int(clip().effects.size()), 3);
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("Kdenlive/glow.xml"));
    editor.addEffect({1}, Presets::Prefix + QDir(dir).filePath("glow/Hell"));
    CHECK_EQ(int(clip().effects.size()), 4); // glow only once, with the later values
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
            if (list->item(i)->text() == "Shut-off") CHECK(list->item(i)->toolTip().contains("qtblend"));
        const auto sub = tree->findItems("Kdenlive", Qt::MatchExactly | Qt::MatchRecursive);
        if (CHECK(sub.size() == 1)) {
            tree->setCurrentItem(sub.first());
            CHECK_EQ(list->count(), 2);
        }
    }
    // ---- Own presets: save as Kdenlive effect group, read back ----
    {
        Clip c;
        c.in = 10;
        c.out = 59; // 50 frames
        CHECK(EffectRegistry::add(c, "frei0r.glow"));
        CHECK(EffectRegistry::add(c, "frei0r.flippo"));
        CHECK(EffectRegistry::add(c, "frei0r.colortap"));
        CHECK(EffectRegistry::add(c, "chromakey"));
        CHECK(EffectRegistry::add(c, "blur"));
        CHECK(EffectRegistry::add(c, "color"));
        CHECK(EffectRegistry::add(c, "grade"));
        c.effects << EffectInstance{"frei0r.gibtsnicht", {{"0", 0.5}}, true};
        EffectRegistry::instance(c, "frei0r.flippo")->params["0"] = true;
        EffectRegistry::instance(c, "frei0r.flippo")->enabled = false;
        EffectRegistry::instance(c, "frei0r.colortap")->params["0"] = "xray";
        EffectRegistry::instance(c, "chromakey")->params["color"] = QColor(1, 2, 3, 200);
        EffectRegistry::instance(c, "chromakey")->params["distance"] = 0.42;
        EffectRegistry::instance(c, "color")->params["brightness"] = 12.5;
        const AnimParam glow = animOf("frei0r.glow", "0");
        Keys::setKey(c, glow, -5, 0.0); // before the clip start (trimmed): becomes a key at frame 0
        Keys::setKey(c, glow, 20, 0.8);
        Keys::setKey(c, glow, 40, 0.3);
        c.keys[glow][1].ease = KeyEase::EaseOut;
        c.keys[glow][2].ease = KeyEase::EaseInOut;
        Keys::setKey(c, AnimParam::FxBlur, 0, 10);
        Keys::setKey(c, AnimParam::FxBlur, 49, 60);
        c.keys[AnimParam::FxBlur][0].ease = KeyEase::Bezier;
        Keys::autoHandles(c.keys[AnimParam::FxBlur], 0);

        QStringList skipped;
        const QByteArray xml = Presets::toXml(c, "Mein Look", &skipped);
        if (qEnvironmentVariableIsSet("PRESET_XML")) qInfo("%s", xml.constData());
        CHECK(xml.contains("<effectgroup") && !xml.contains("mlt_service"));
        CHECK(kdenliveAccepts(xml)); // own effects (blur, color) do not make Kdenlive refuse the group
        CHECK(xml.contains(R"(<property name="Blur">)"));      // Kdenlive's name of the glow parameter
        CHECK(xml.contains(R"(<schneidi-effect id="color">)"));
        CHECK_EQ(skipped, (QStringList{EffectRegistry::find("grade")->name, "frei0r.gibtsnicht"}));
        const Presets::Preset p = Presets::parse(xml, "Mein Look.xml");
        CHECK(p.error.isEmpty());
        CHECK_EQ(p.name, QString("Mein Look"));
        CHECK_EQ(p.source, QString("Kdenlive"));
        const Presets::Mapped m = Presets::map(p);
        CHECK(m.skipped.isEmpty() && !m.keyframes);
        // Onto another clip (other in point): same look at every frame
        Clip t;
        t.in = 100;
        t.out = 149;
        Presets::apply(t, m);
        QStringList ids;
        for (const EffectInstance& e : t.effects) ids << e.effectId;
        CHECK_EQ(ids, (QStringList{"frei0r.glow", "frei0r.flippo", "frei0r.colortap", "chromakey", "blur", "color"}));
        for (const EffectInstance& e : t.effects) {
            const EffectInstance* o = EffectRegistry::instance(c, e.effectId);
            if (!CHECK(o)) continue;
            CHECK_EQ(e.enabled, o->enabled);
            for (auto it = o->params.begin(); it != o->params.end(); ++it) {
                if (const AnimParam a = animOf(e.effectId, it.key()); a != AnimParam::Count && Keys::animated(c, a))
                    continue; // animated: compared below
                if (it.value().typeId() == QMetaType::Double)
                    CHECK(qAbs(e.params.value(it.key()).toDouble() - it.value().toDouble()) < 1e-9);
                else
                    CHECK(e.params.value(it.key()) == it.value());
            }
        }
        CHECK(!EffectRegistry::instance(t, "frei0r.flippo")->enabled);
        double worst = 0;
        for (int f = 0; f < 50; ++f) worst = std::max(worst, std::abs(Keys::valueAt(t, glow, f) - Keys::valueAt(c, glow, f)));
        CHECK(worst < 1e-6); // eases survive as MLT sinusoidal types
        CHECK_EQ(Keys::keyTimes(t, {glow}), (QVector<int>{0, 20, 40}));
        CHECK_EQ(t.keys.value(glow).value(0).frame, 100);
        if (CHECK(Keys::animated(t, AnimParam::FxBlur))) { // Bezier -> smooth: same ends, soft curve
            CHECK(t.keys[AnimParam::FxBlur][0].ease == KeyEase::Bezier);
            CHECK(Keys::valueAt(t, AnimParam::FxBlur, 0) == 10 && Keys::valueAt(t, AnimParam::FxBlur, 49) == 60);
        }

        // Save into the presets folder (safe file name), apply via the Editor, listed in the Library
        const QString path = Presets::userPresetPath("Mein/Look: *1*");
        CHECK_EQ(QFileInfo(path).absolutePath(), QFileInfo(EffectFolders::presetDir()).absoluteFilePath());
        CHECK_EQ(QFileInfo(path).fileName(), QString("Mein_Look_ _1_.xml"));
        QString err;
        CHECK(Presets::save(c, "Mein Look", path, &err));
        project.edit("clear", [](Timeline& tl) { tl.video[0].clips[0].effects.clear(); tl.video[0].clips[0].keys.clear(); });
        editor.addEffect({1}, Presets::Prefix + path);
        CHECK_EQ(int(clip().effects.size()), 6);
        CHECK(Keys::animated(clip(), glow));
        EffectsLibrary lib2;
        auto* tree2 = lib2.findChild<QTreeWidget*>();
        auto* list2 = lib2.findChild<QListWidget*>();
        const auto top = tree2->findItems("Presets", Qt::MatchExactly | Qt::MatchRecursive);
        if (CHECK(top.size() == 1)) {
            tree2->setCurrentItem(top.first());
            bool listed = false;
            for (int i = 0; i < list2->count(); ++i)
                listed |= list2->item(i)->text() == "Mein Look" && !list2->item(i)->data(Qt::UserRole).toString().isEmpty();
            CHECK(listed);
        }
    }

    // ---- Own preset with one MLT effect: Kdenlive custom effect (<effect tag> with <parameter> definitions) ----
    {
        Clip c;
        c.in = 0;
        c.out = 49;
        CHECK(EffectRegistry::add(c, "frei0r.glow"));
        CHECK(EffectRegistry::add(c, "color"));
        EffectRegistry::instance(c, "color")->params["saturation"] = -40.0;
        const AnimParam glow = animOf("frei0r.glow", "0");
        Keys::setKey(c, glow, 0, 0.1);
        Keys::setKey(c, glow, 30, 0.9);
        const QByteArray xml = Presets::toXml(c, "Ein Glow");
        if (qEnvironmentVariableIsSet("PRESET_XML")) qInfo("%s", xml.constData());
        CHECK(kdenliveAccepts(xml));
        CHECK(xml.contains(R"(<effect tag="frei0r.glow" id="Ein Glow" type="customVideo">)"));
        CHECK(xml.contains(R"(<parameter type="animated" name="Blur" default="0" min="0" max="1" decimals="3" value="0=0.1;30=0.9">)"));
        CHECK(xml.contains(R"(<schneidi-effect id="color">)"));
        const Presets::Preset p = Presets::parse(xml, "Ein Glow.xml");
        CHECK_EQ(p.name, QString("Ein Glow"));
        const Presets::Mapped m = Presets::map(p);
        CHECK(m.skipped.isEmpty());
        if (const EffectInstance* e = effect(m, "color"); CHECK(e)) CHECK(qAbs(num(*e, "saturation") + 40) < 1e-9);
        if (CHECK(m.keys.contains(glow))) CHECK(m.keys[glow].size() == 2 && m.keys[glow][1].frame == 30);
        // Disabled: only schneidi reads it back
        EffectRegistry::instance(c, "frei0r.glow")->enabled = false;
        const Presets::Mapped off = Presets::map(Presets::parse(Presets::toXml(c, "x"), "x.xml"));
        if (const EffectInstance* e = effect(off, "frei0r.glow"); CHECK(e)) CHECK(!e->enabled);
    }
    // Plugins whose Kdenlive file uses indexes / old names keep them
    if (EffectRegistry::find("frei0r.rgbsplit0r") && EffectRegistry::find("frei0r.lenscorrection")) {
        Clip c;
        c.out = 9;
        CHECK(EffectRegistry::add(c, "frei0r.rgbsplit0r"));
        CHECK(EffectRegistry::add(c, "frei0r.lenscorrection"));
        const QByteArray xml = Presets::toXml(c, "Namen");
        CHECK(kdenliveAccepts(xml));
        CHECK(xml.contains(R"(<property name="1">)") && xml.contains(R"(<property name="xcenter">)"));
        CHECK(xml.contains(R"(<property name="brightness">)"));
    }

    QDir(EffectFolders::root()).removeRecursively();
    return Check::result();
}
