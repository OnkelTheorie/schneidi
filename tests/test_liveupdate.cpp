// Test LiveUpdate: Inspector-style value changes copied into a running tractor give the same picture as a fresh
// build (transform, color correction, blur, Color page grade, frei0r, keyframes, fades, dissolve); structural
// changes (trim, added effect) are refused.
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFormat.h"
#include "engine/LiveUpdate.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <clocale>
#include <cstring>
#include <functional>

namespace {

constexpr int kW = 480, kH = 270;

QImage render(Mlt::Tractor& tr, int pos)
{
    tr.seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr.get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    int w = kW, h = kH;
    const uint8_t* d = f->get_image(fmt, w, h);
    if (!d || w != kW || h != kH) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img.copy();
}

double diff(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return 255;
    const uint8_t* x = a.constBits();
    const uint8_t* y = b.constBits();
    const size_t n = size_t(a.width()) * a.height() * 4;
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += std::abs(int(x[i]) - int(y[i]));
    return s / n;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("liveupdate");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    QTemporaryDir tmp;
    const QString media = Check::makeMedia(tmp.filePath("bars.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-c:v",
                                            "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (media.isEmpty()) return Check::skip("Testmedium");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    // a frei0r filter with a number parameter (optional: frei0r may be missing)
    QString f0r, f0rKey;
    for (const EffectDescriptor& d : EffectRegistry::all()) {
        if (d.category != EffectRegistry::Frei0rCategory || !d.video || d.id.contains("bluescreen")) continue;
        for (const EffectParam& p : d.params)
            if (p.type == EffectParam::Double && p.max > p.min && f0rKey.isEmpty()) f0rKey = p.key;
        if (!f0rKey.isEmpty()) {
            f0r = d.id;
            break;
        }
    }

    // V1: two clips with a cross dissolve; the first with every kind of effect and keyframes
    Timeline base;
    base.video.resize(1);
    Clip a;
    a.id = 1;
    a.mediaPath = media;
    a.in = 0;
    a.out = 59;
    a.transform.zoomX = a.transform.zoomY = 1.2;
    a.transform.rotation = 5;
    a.fadeIn = 10;
    EffectRegistry::add(a, "color");
    EffectRegistry::instance(a, "color")->params["brightness"] = 10.0;
    EffectRegistry::add(a, "blur");
    EffectRegistry::instance(a, "blur")->params["strength"] = 5.0;
    EffectRegistry::add(a, "grade");
    Keys::setStaticValue(a, AnimParam::GradeSaturation, 70);
    if (!f0r.isEmpty()) EffectRegistry::add(a, f0r);
    Keys::setKey(a, AnimParam::PosX, 0, 0);
    Keys::setKey(a, AnimParam::PosX, 59, 100);
    Clip b = a;
    b.id = 2;
    b.start = 60;
    b.in = 60;
    b.out = 99;
    b.effects.clear();
    b.keys.clear();
    b.fadeIn = 0;
    a.transOut = 10;
    b.transIn = 10;
    base.video[0].clips << a << b;

    const ProjectFormat fmt;
    auto prof = makeProfile(fmt);

    struct Case {
        const char* name;
        std::function<void(Clip&)> change;
        int pos;
    };
    const QVector<Case> cases = {
        {"transform", [](Clip& c) { c.transform.zoomX = 1.5; c.transform.posY = 40; c.transform.rotation = 20; }, 30},
        {"color", [](Clip& c) { EffectRegistry::instance(c, "color")->params["brightness"] = -40.0; }, 30},
        {"blur", [](Clip& c) { EffectRegistry::instance(c, "blur")->params["strength"] = 40.0; }, 30},
        {"grade", [](Clip& c) { Keys::setStaticValue(c, AnimParam::GradeSaturation, 0); }, 30},
        {"grade key", [](Clip& c) {
             Keys::setKey(c, AnimParam::GradeGainR, 0, 0);
             Keys::setKey(c, AnimParam::GradeGainR, 59, 0.5);
         }, 40},
        {"keyframe", [](Clip& c) { Keys::setValue(c, AnimParam::PosX, 59, -200); }, 45},
        {"fade", [](Clip& c) { c.fadeIn = 30; }, 5},
        {"in dissolve", [](Clip& c) { c.transform.posY = -120; }, 57},
    };
    for (const Case& t : cases) {
        Timeline changed = base;
        t.change(changed.video[0].clips[0]);
        CHECK(LiveUpdate::valuesOnly(base, changed));

        TimelineBuilder builder(*prof);
        MixerHooks h1, h2;
        auto live = builder.build(base, &h1);
        const QImage before = render(*live, t.pos);
        auto fresh = builder.build(changed, &h2);
        const bool ok = LiveUpdate::transfer(*live, *fresh);
        Check::report(ok, t.name, __FILE__, __LINE__, "transfer refused");
        fresh.reset();
        const QImage after = render(*live, t.pos);

        TimelineBuilder other(*prof);
        auto ref = other.build(changed);
        const QImage expected = render(*ref, t.pos);
        Check::report(diff(after, expected) < 0.5, t.name, __FILE__, __LINE__,
                      QString("live vs fresh build: %1").arg(diff(after, expected)));
        Check::report(diff(before, expected) > 1.0, t.name, __FILE__, __LINE__,
                      QString("change not visible: %1").arg(diff(before, expected)));
    }

    if (!f0r.isEmpty()) { // frei0r parameter: same picture as a fresh build
        Timeline changed = base;
        const EffectDescriptor* d = EffectRegistry::find(f0r);
        double v = 0;
        for (const EffectParam& p : d->params)
            if (p.key == f0rKey) v = p.min + (p.max - p.min) * 0.9;
        EffectRegistry::instance(changed.video[0].clips[0], f0r)->params[f0rKey] = v;
        CHECK(LiveUpdate::valuesOnly(base, changed));
        TimelineBuilder builder(*prof);
        auto live = builder.build(base);
        render(*live, 30);
        auto fresh = builder.build(changed);
        CHECK(LiveUpdate::transfer(*live, *fresh));
        TimelineBuilder other(*prof);
        auto ref = other.build(changed);
        CHECK(diff(render(*live, 30), render(*ref, 30)) < 0.5);
    }

    // structural changes are not values
    {
        Timeline trimmed = base;
        trimmed.video[0].clips[1].out -= 5;
        CHECK(!LiveUpdate::valuesOnly(base, trimmed));
        Timeline added = base;
        EffectRegistry::add(added.video[0].clips[1], "blur");
        CHECK(!LiveUpdate::valuesOnly(base, added));
        Timeline title = base;
        title.video[0].clips[0].enabled = false;
        CHECK(!LiveUpdate::valuesOnly(base, title));
    }
    // values that change the graph (a filter appears) are refused by transfer()
    {
        Timeline noRotation = base, rotated = base;
        noRotation.video[0].clips[1].transform = {};
        rotated.video[0].clips[1].transform.rotation = 30;
        CHECK(LiveUpdate::valuesOnly(noRotation, rotated));
        TimelineBuilder builder(*prof);
        auto live = builder.build(noRotation);
        auto fresh = builder.build(rotated);
        CHECK(!LiveUpdate::transfer(*live, *fresh));
    }
    return Check::result();
}
