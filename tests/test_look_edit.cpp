// SPDX-License-Identifier: AGPL-3.0-only
// Test the bundled look "Edit" (assets/presets/edit.xml = LUT assets/luts/edit.cube + a light glow): listed in the
// Effects Library and by `effects`, found by name for the `effect` op, applied as one undo step, and its pixels go the
// expected way (shadows teal, highlights orange, saturation up but skin not neon); the glow's strength keeps it mild.
// Before/after pictures: LOOK_EDIT_DUMP=<dir> build/tests/test_look_edit
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "engine/ColorGrade.h"
#include "engine/VideoEffects.h"

#include <QApplication>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

const QString kLook = QStringLiteral(":/presets/edit.xml");
const QString kLut = QStringLiteral(":/luts/edit.cube");

struct Rgb {
    int r, g, b;
};

Rgb graded(const ColorGrade::Lut& lut, Rgb in)
{
    uint8_t px[4] = {uint8_t(in.r), uint8_t(in.g), uint8_t(in.b), 255};
    ColorGrade::apply(px, 1, 1, ColorGrade::Params(), &lut);
    return {px[0], px[1], px[2]};
}

double saturation(Rgb c) // HSV-like: (max - min) / max
{
    const int mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    return mx ? double(mx - mn) / mx : 0;
}

// The look's own glow values from the preset
const EffectInstance* glowOf(const Presets::Mapped& m)
{
    for (const EffectInstance& e : m.effects)
        if (e.effectId == "frei0r.glow") return &e;
    return nullptr;
}

void testListed()
{
    const auto looks = EffectFolders::builtinPresets();
    const auto it = std::find_if(looks.begin(), looks.end(), [](const auto& l) { return l.path == kLook; });
    if (!CHECK(it != looks.end())) return;
    CHECK(!it->name.isEmpty() && EffectFolders::isBuiltin(kLook));
    CHECK_EQ(EffectFolders::builtinId(kLook), QString("edit"));
    CHECK_EQ(EffectFolders::displayName(kLook), it->name);
    CHECK(!EffectFolders::builtinPresetDescription(kLook).isEmpty());
    // The color alone is a bundled LUT too
    const auto luts = EffectFolders::builtinLuts();
    CHECK(std::any_of(luts.begin(), luts.end(), [](const auto& l) { return l.path == kLut; }));

    // The preset: translated name, its LUT and a mild glow
    const Presets::Preset p = Presets::load(kLook);
    CHECK(p.error.isEmpty());
    CHECK_EQ(p.name, it->name);
    CHECK_EQ(p.source, QString("schneidi"));
    const Presets::Mapped m = Presets::map(p);
    CHECK(m.skipped.isEmpty());
    CHECK_EQ(m.lut, kLut);
    if (const EffectInstance* g = glowOf(m); CHECK(g)) {
        CHECK(g->params.value("0").toDouble() > 0.05);
        const double strength = g->params.value("strength").toDouble();
        CHECK(strength > 0.05 && strength <= 0.35);
    }

    // CLI: `effects` lists it under presets (with a description), the `effect` op finds it by id
    const Cli::Command* cmd = Cli::find("effects");
    if (!CHECK(cmd)) return;
    Cli::Context ctx;
    const QJsonObject out = cmd->run(Cli::normalize(*cmd, {}), ctx);
    bool found = false;
    for (const QJsonValue& v : out.value("presets").toArray())
        if (v.toObject().value("name").toString() == "edit") found = !v.toObject().value("description").toString().isEmpty();
    CHECK(found);
    bool lutFound = false;
    for (const QJsonValue& v : out.value("luts").toArray()) lutFound |= v.toObject().value("name").toString() == "edit";
    CHECK(lutFound);
    CHECK_EQ(Cli::detail::findAsset(Cli::detail::Asset::Preset, "edit"), kLook);
    CHECK_EQ(Cli::detail::findAsset(Cli::detail::Asset::Preset, it->name), kLook);
}

void testOneUndoStep()
{
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
    editor.addEffect({1}, Presets::Prefix + kLook);
    CHECK_EQ(project.undoStack()->count(), steps + 1);
    CHECK_EQ(ColorGrade::lutPath(clip()), kLut);
    CHECK(EffectRegistry::has(clip(), "frei0r.glow"));
    CHECK(EffectRegistry::value(clip(), "frei0r.glow", "strength").toDouble() < 0.5);
    project.undoStack()->undo();
    CHECK(ColorGrade::lutPath(clip()).isEmpty());
    CHECK(clip().effects.isEmpty());
}

void testColors()
{
    QString err;
    const auto lut = ColorGrade::parseLut(kLut, &err);
    if (!CHECK(lut)) {
        std::printf("  %s\n", qPrintable(err));
        return;
    }
    // Shadows teal: a dark gray gets more blue and green than red
    const Rgb dark = graded(*lut, {50, 50, 50});
    CHECK(dark.b > dark.r + 4 && dark.g > dark.r);
    // Highlights orange: a light gray gets more red than blue
    const Rgb light = graded(*lut, {200, 200, 200});
    CHECK(light.r > light.b + 4);
    // Mild: no gray moves far
    CHECK(std::abs(dark.b - 50) < 25 && std::abs(light.r - 200) < 25);
    // Dull colours get more saturated (a muted green, a muted blue)
    for (const Rgb c : {Rgb{110, 140, 100}, Rgb{100, 115, 150}})
        CHECK(saturation(graded(*lut, c)) > saturation(c) + 0.03);
    // Skin stays near its saturation (no neon), much less boosted than the muted green
    const Rgb skin{205, 150, 120};
    const double skinGain = saturation(graded(*lut, skin)) - saturation(skin);
    const Rgb green{110, 140, 100};
    const double greenGain = saturation(graded(*lut, green)) - saturation(green);
    CHECK(skinGain < 0.12 && skinGain < greenGain);
    // Saturated colours are not pushed to the edge (vibrance)
    const Rgb red = graded(*lut, {220, 40, 40});
    CHECK(red.g > 15 && red.b > 15);
    // Black and white stay
    const Rgb black = graded(*lut, {0, 0, 0}), white = graded(*lut, {255, 255, 255});
    CHECK(std::max({black.r, black.g, black.b}) < 12 && std::min({white.r, white.g, white.b}) > 235);
}

void testGlowMild()
{
    const Presets::Mapped m = Presets::map(Presets::load(kLook));
    const EffectInstance* g = glowOf(m);
    if (!CHECK(g)) return;
    const double blur = g->params.value("0").toDouble(), strength = g->params.value("strength").toDouble();
    // A dark picture with a bright spot in the middle
    const int w = 320, h = 180;
    std::vector<uint8_t> base(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = base.data() + (size_t(y) * w + x) * 4;
            const bool spot = std::abs(x - w / 2) < 12 && std::abs(y - h / 2) < 12;
            p[0] = p[1] = p[2] = spot ? 250 : 30;
            p[3] = 255;
        }
    auto at = [&](const std::vector<uint8_t>& px, int x, int y) { return int(px[(size_t(y) * w + x) * 4]); };
    std::vector<uint8_t> look = base, full = base;
    const double threshold = g->params.value("threshold").toDouble();
    CHECK(threshold > 0.3 && threshold < 0.9);
    VideoEffects::glow(look.data(), w, h, blur, strength, threshold);
    VideoEffects::glow(full.data(), w, h, blur, 1.0);
    // Next to the spot it gets brighter, but only mildly (a fraction of the plugin's full glow)
    const int nx = w / 2 + 16, ny = h / 2;
    const int lift = at(look, nx, ny) - at(base, nx, ny), fullLift = at(full, nx, ny) - at(base, nx, ny);
    CHECK(lift >= 3 && lift < 40);
    CHECK(lift * 2 < fullLift);
    // Far from the spot nothing changes (the threshold: no haze over dark parts), the spot itself stays bright
    const int farLift = at(look, 5, 5) - at(base, 5, 5);
    if (!CHECK(farLift >= 0 && farLift <= 1 && lift >= 3)) std::printf("  lift %d, far %d, full %d\n", lift, farLift, fullLift);
    CHECK(at(look, w / 2, h / 2) >= 245);
    // strength 1, threshold 0 = the old glow (as before the parameters existed), strength 0 = nothing
    std::vector<uint8_t> old = base, none = base;
    VideoEffects::glow(old.data(), w, h, blur);
    CHECK(old == full);
    VideoEffects::glow(none.data(), w, h, blur, 0.0);
    CHECK(none == base);
    // By effect id: without the "strength"/"threshold" keys (older projects) the full glow
    std::vector<uint8_t> byId = base;
    VideoEffects::Params params;
    params.has = [](const char* k) { return QByteArray(k) == "0"; };
    params.num = [&](const char*) { return blur; };
    CHECK(VideoEffects::apply("frei0r.glow", byId.data(), w, h, params, 0));
    CHECK(byId == full);
}

// Before/after pictures of the LUT and the whole look on a synthetic test card (gray ramp, colour bars, skin patches)
void dump(const QString& dir)
{
    const int w = 640, h = 360;
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            QColor c;
            if (y < 90) c = QColor::fromHsvF(x / double(w), 0.55, 0.8);           // muted hues
            else if (y < 180) c = QColor::fromRgbF(x / double(w - 1), x / double(w - 1), x / double(w - 1)); // ramp
            else if (y < 270) { // skin tones, light to dark
                static const QColor skins[] = {QColor(241, 194, 167), QColor(224, 172, 140), QColor(198, 134, 99),
                                               QColor(160, 104, 72), QColor(110, 70, 50)};
                c = skins[std::min(4, x * 5 / w)];
            } else c = (x / 40) % 2 ? QColor(20, 24, 30) : QColor(60, 80, 60); // dark scene
            img.setPixelColor(x, y, c);
        }
    for (int y = 280; y < 330; ++y) // a bright light in the dark scene
        for (int x = 290; x < 350; ++x) img.setPixelColor(x, y, QColor(255, 245, 225));
    img.save(QDir(dir).filePath("look_edit_before.png"));
    const auto lut = ColorGrade::parseLut(kLut);
    QImage after = img.copy();
    ColorGrade::apply(after.bits(), w, h, ColorGrade::Params(), lut.get());
    after.save(QDir(dir).filePath("look_edit_lut.png"));
    const EffectInstance* g = glowOf(Presets::map(Presets::load(kLook)));
    VideoEffects::glow(after.bits(), w, h, g->params.value("0").toDouble(), g->params.value("strength").toDouble(),
                       g->params.value("threshold").toDouble());
    after.save(QDir(dir).filePath("look_edit_after.png"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("look_edit");
    Check::initEngine();
    testListed();
    testOneUndoStep();
    testColors();
    testGlowMild();
    if (const QByteArray d = qgetenv("LOOK_EDIT_DUMP"); !d.isEmpty()) dump(QString::fromLocal8Bit(d));
    return Check::result();
}
