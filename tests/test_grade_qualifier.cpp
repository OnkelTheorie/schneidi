// SPDX-License-Identifier: AGPL-3.0-only
// Color page 2 (0.24): curves (core/GradeCurves: text form, monotone shape, hue wrap), qualifier (core/GradeQualifier:
// measures, soft ranges, a range around a picked colour), the grade's pixel work with them (engine/ColorGrade: the
// baked 3D table against the exact per-pixel reference, mask view, LUT after the qualifier, cache), editor ops with
// undo, project file round trip (old projects without them unchanged), CLI ops `color curves` and `qualifier`, the
// Color panel (palettes, curve editor drag = one undo step, qualifier layer, mask signal) and a render through the
// picture engine (preview mask vs export picture).
#include "cli/Commands.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/GradeCurves.h"
#include "core/GradeQualifier.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/GradedThumbs.h"
#include "engine/picture/Renderer.h"
#include "app/Theme.h"
#include "ui/ColorPanel.h"
#include "ui/CurveEditor.h"
#include "ui/WindowOverlay.h"
#include "core/GradeStills.h"

#include "render_check.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QDir>
#include <QJsonArray>
#include <QListWidget>
#include <QPainter>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include <atomic>
#include <clocale>
#include <cmath>
#include <vector>

namespace {

using K = GradeCurves::Kind;
using CP = ColorGrade::Params;

void testCurves()
{
    // Text form: sorted, clamped, merged, C locale
    const GradeCurves::Points p = GradeCurves::parse("1,1 0,0 0.5,0.7 0.501,0.6 2,-1");
    CHECK_EQ(p.size(), 3);
    if (p.size() == 3) {
        CHECK_EQ(p[1], QPointF(0.501, 0.6)); // the later of two close points wins
        CHECK_EQ(p[2], QPointF(1, 0));       // clamped (and merged with the 1,1 before it)
    }
    CHECK(GradeCurves::parse("0,0 nonsense").isEmpty());
    CHECK_EQ(GradeCurves::format({{0, 0}, {0.25, 0.3}, {1, 1}}), QString("0,0 0.25,0.3 1,1"));
    CHECK(GradeCurves::parse(GradeCurves::format({{0.1, 0.2}, {0.9, 0.95}})) == (GradeCurves::Points{{0.1, 0.2}, {0.9, 0.95}}));

    // Identity
    CHECK(GradeCurves::isIdentity({}, K::Y));
    CHECK(GradeCurves::isIdentity({{0, 0}, {0.5, 0.5}, {1, 1}}, K::R));
    CHECK(!GradeCurves::isIdentity({{0, 0}, {0.5, 0.5}}, K::Y)); // flat beyond 0.5
    CHECK(GradeCurves::isIdentity({{0.3, 0.5}}, K::HueSat));
    CHECK_EQ(GradeCurves::eval({}, K::HueHue, 0.3), 0.5);
    CHECK_EQ(GradeCurves::eval({}, K::G, 0.3), 0.3);

    // Through the points, monotone between (an S curve never dips below its points), flat outside
    const GradeCurves::Points s{{0, 0}, {0.25, 0.15}, {0.75, 0.85}, {1, 1}};
    CHECK(std::abs(GradeCurves::eval(s, K::Y, 0.25) - 0.15) < 1e-9);
    double last = -1;
    bool monotone = true;
    for (int i = 0; i <= 100; ++i) {
        const double v = GradeCurves::eval(s, K::Y, i / 100.0);
        monotone &= v >= last - 1e-12;
        last = v;
    }
    CHECK(monotone);
    CHECK_EQ(GradeCurves::eval({{0.2, 0.3}, {0.8, 0.6}}, K::Y, 0.05), 0.3);
    // Hue curve wraps: a point near 1 pulls the value near 0
    const GradeCurves::Points h{{0.05, 0.9}, {0.5, 0.5}, {0.95, 0.9}};
    CHECK(std::abs(GradeCurves::eval(h, K::HueSat, 0.0) - 0.9) < 0.05);
    CHECK(std::abs(GradeCurves::eval(h, K::HueSat, 1.0) - GradeCurves::eval(h, K::HueSat, 0.0)) < 1e-9);
    // 0 and 1 at once (the same hue) do not break it
    CHECK(std::isfinite(GradeCurves::eval({{0, 0.7}, {0.5, 0.5}, {1, 0.7}}, K::HueHue, 0.99)));
    CHECK_EQ(GradeCurves::table(s, K::Y, 4).size(), size_t(5));
    // No seam at red: the segment across the wrap has the same tangents on both ends (review 0.24: 0.470 vs 0.282)
    const GradeCurves::Points w{{0.05, 0.9}, {0.2, 0.5}, {0.8, 0.5}, {0.97, 0.1}};
    for (K k : {K::HueHue, K::HueSat}) {
        CHECK(std::abs(GradeCurves::eval(w, k, 0.0) - GradeCurves::eval(w, k, 1 - 1e-7)) < 1e-4);
        // and smooth: the slope just before 1 equals the slope just after 0
        const double e = 1e-6;
        const double before = (GradeCurves::eval(w, k, 1 - e) - GradeCurves::eval(w, k, 1 - 2 * e)) / e;
        const double after = (GradeCurves::eval(w, k, 2 * e) - GradeCurves::eval(w, k, e)) / e;
        CHECK(std::abs(before - after) < 0.02 * std::max(1.0, std::abs(before)));
    }
    CHECK(std::abs(GradeCurves::eval(w, K::HueSat, 0.05) - 0.9) < 1e-9); // still through its points
    // A prepared shape gives the same values as eval
    const GradeCurves::Shape shape(s, K::Y);
    CHECK(!shape.identity() && std::abs(shape(0.4) - GradeCurves::eval(s, K::Y, 0.4)) < 1e-12);
    CHECK(GradeCurves::Shape({}, K::HueSat).identity());
}

void testQualifierMath()
{
    double h, s, l;
    GradeQualifier::measure(1, 0, 0, &h, &s, &l);
    CHECK(std::abs(h) < 1e-9 && std::abs(s - 100) < 1e-9 && std::abs(l - 21.26) < 1e-6);
    GradeQualifier::measure(0, 0, 1, &h, &s, &l);
    CHECK(std::abs(h - 240) < 1e-6);
    GradeQualifier::Range r;
    r.hue = 0;
    r.hueWidth = 40;
    r.hueSoft = 20;
    r.satLow = 20;
    r.satHigh = 100;
    r.satSoft = 0;
    r.lumLow = 0;
    r.lumHigh = 100;
    CHECK_EQ(r.weight(1, 0, 0), 1.0f);         // red: inside
    CHECK_EQ(r.weight(0, 1, 0), 0.0f);         // green: far away
    CHECK_EQ(r.weight(0.5f, 0.5f, 0.5f), 0.0f); // gray: below the saturation
    // Soft edge across red (hue 350 = 10 degrees off: inside the width; 30 degrees off: half way in the soft part)
    const QColor at350 = QColor::fromHsvF(350 / 360.0, 1, 1), at330 = QColor::fromHsvF(330 / 360.0, 1, 1);
    CHECK_EQ(r.weight(at350.redF(), at350.greenF(), at350.blueF()), 1.0f);
    const float mid = r.weight(at330.redF(), at330.greenF(), at330.blueF());
    CHECK(mid > 0.3f && mid < 0.7f);
    r.invert = true;
    CHECK_EQ(r.weight(1, 0, 0), 0.0f);
    // Around a picked colour: that colour is inside
    const QColor skin(200, 150, 120);
    const GradeQualifier::Range a = GradeQualifier::around(skin);
    CHECK_EQ(a.weight(skin.redF(), skin.greenF(), skin.blueF()), 1.0f);
    CHECK(a.weight(0.2f, 0.4f, 0.9f) == 0.0f);
    const GradeQualifier::Range g = GradeQualifier::around(QColor(128, 128, 128));
    CHECK_EQ(g.hueWidth, 360.0);
}

std::vector<uint8_t> testPicture()
{
    std::vector<uint8_t> px;
    for (int r = 0; r < 256; r += 15)
        for (int g = 0; g < 256; g += 15)
            for (int b = 0; b < 256; b += 15) px.insert(px.end(), {uint8_t(r), uint8_t(g), uint8_t(b), 255});
    for (int i = 0; i < 256; ++i) px.insert(px.end(), {uint8_t(i), uint8_t(i * 3 / 4), uint8_t(i / 2), 200}); // skin ramp
    return px;
}

// Baked (3D table) against the exact per-pixel reference: max and mean difference
void compareBaked(const char* name, const CP& p, const ColorGrade::Lut* lut, int maxTol, double meanTol)
{
    std::vector<uint8_t> a = testPicture(), b = a;
    const int w = int(a.size() / 4);
    ColorGrade::apply(a.data(), w, 1, *ColorGrade::bake(p, lut));
    ColorGrade::applyReference(b.data(), w, 1, p, lut);
    int mx = 0;
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (i % 4 == 3) {
            CHECK(a[i] == b[i]); // alpha stays
            continue;
        }
        const int d = std::abs(int(a[i]) - int(b[i]));
        mx = std::max(mx, d);
        sum += d;
    }
    const double mean = sum / (a.size() * 3 / 4);
    std::printf("  %-28s max %d mean %.3f\n", name, mx, mean);
    CHECK(mx <= maxTol);
    CHECK(mean <= meanTol);
}

uint8_t one(const CP& p, int r, int g, int b, int c)
{
    uint8_t px[4] = {uint8_t(r), uint8_t(g), uint8_t(b), 255};
    ColorGrade::apply(px, 1, 1, *ColorGrade::bake(p, nullptr));
    return px[c];
}

void testPixels()
{
    // Tone curves: exact per byte (no 3D table needed)
    CP p;
    p.curves.points[int(K::Y)] = {{0, 0}, {0.5, 0.7}, {1, 1}};
    CHECK(!p.isNeutral());
    CHECK(std::abs(int(one(p, 128, 128, 128, 0)) - 178) <= 1);
    CHECK_EQ(int(one(p, 0, 0, 0, 1)), 0);
    compareBaked("tone curves", p, nullptr, 1, 0.1);
    CP r;
    r.curves.points[int(K::R)] = {{0, 0.2}, {1, 1}};
    CHECK(one(r, 0, 0, 0, 0) >= 50 && one(r, 0, 0, 0, 1) == 0);

    // Hue vs saturation: blue gray, red stays
    CP hs;
    hs.curves.points[int(K::HueSat)] = {{0.5, 0.5}, {0.667, 0.0}, {0.85, 0.5}};
    const uint8_t bb = one(hs, 40, 40, 200, 0), bg = one(hs, 40, 40, 200, 2);
    CHECK(std::abs(int(bb) - int(bg)) <= 3);
    CHECK_EQ(int(one(hs, 200, 40, 40, 0)), 200);
    compareBaked("hue vs saturation", hs, nullptr, 6, 0.6);
    // Hue vs hue keeps the brightness
    CP hh;
    hh.curves.points[int(K::HueHue)] = {{0.2, 0.5}, {0.333, 0.6}, {0.45, 0.5}};
    compareBaked("hue vs hue", hh, nullptr, 8, 0.8);
    // Hue vs hue turns by (y - 0.5) * 360 degrees of HSV hue, brightness kept: red + 120 degrees = green
    {
        CP turn;
        turn.curves.points[int(K::HueHue)] = {{0.0, 0.5 + 1.0 / 3}, {0.5, 0.5 + 1.0 / 3}};
        float v[3] = {0.8f, 0.2f, 0.2f};
        uint8_t px[4] = {204, 51, 51, 255};
        ColorGrade::applyReference(px, 1, 1, turn, nullptr);
        double h, sat, l, h0, s0, l0;
        GradeQualifier::measure(px[0] / 255.f, px[1] / 255.f, px[2] / 255.f, &h, &sat, &l);
        GradeQualifier::measure(v[0], v[1], v[2], &h0, &s0, &l0);
        CHECK(std::abs(h - 120) < 3);
        CHECK(std::abs(l - l0) < 1.0);
    }
    // Tone curves are cheap to bake (the review found 5-10 ms per bake: a 4097-entry table per channel)
    {
        CP tone;
        tone.curves.points[int(K::Y)] = {{0, 0}, {0.3, 0.2}, {0.7, 0.85}, {1, 1}};
        tone.curves.points[int(K::R)] = {{0, 0.05}, {1, 0.95}};
        tone.curves.points[int(K::B)] = {{0, 0}, {0.5, 0.45}, {1, 1}};
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < 50; ++i) ColorGrade::bake(tone, nullptr);
        const double ms = timer.nsecsElapsed() / 1e6 / 50;
        std::printf("  bake with tone curves: %.3f ms\n", ms);
        CHECK(ms < 2.0);
    }
    CP ls;
    ls.curves.points[int(K::LumSat)] = {{0, 0}, {0.5, 0.5}, {1, 0.5}};
    compareBaked("lum vs saturation", ls, nullptr, 6, 0.6);

    // Qualifier: only the selected colours change
    CP q;
    q.qualifier.on = true;
    q.qualifier.range.hue = 0;
    q.qualifier.range.hueWidth = 40;
    q.qualifier.range.hueSoft = 10;
    q.qualifier.range.satLow = 30;
    q.qualifier.range.satHigh = 100;
    q.qualifier.range.lumLow = 0;
    q.qualifier.range.lumHigh = 100;
    CHECK(q.isNeutral()); // a neutral correction in it does nothing
    q.qualifier.correction.saturation = 0;
    CHECK(!q.isNeutral());
    CHECK(std::abs(int(one(q, 220, 30, 30, 0)) - int(one(q, 220, 30, 30, 1))) <= 3); // red: gray
    CHECK_EQ(int(one(q, 30, 200, 30, 1)), 200);                                       // green: unchanged
    CHECK_EQ(int(one(q, 30, 200, 30, 0)), 30);
    compareBaked("qualifier", q, nullptr, 12, 0.8);
    // Mask view: white where selected, black elsewhere
    CP m = q;
    m.view = ColorGrade::View::QualifierMask;
    CHECK(one(m, 220, 30, 30, 0) >= 250 && one(m, 220, 30, 30, 2) >= 250);
    CHECK(one(m, 30, 200, 30, 0) <= 3);
    // The mask with a neutral correction still shows
    CP mn = m;
    mn.qualifier.correction = {};
    CHECK(!mn.isNeutral());
    CHECK(one(mn, 220, 30, 30, 1) >= 250);
    // Inverted: everything but red
    CP inv = q;
    inv.qualifier.range.invert = true;
    CHECK_EQ(int(one(inv, 220, 30, 30, 0)), 220);
    CHECK(std::abs(int(one(inv, 30, 200, 30, 0)) - int(one(inv, 30, 200, 30, 1))) <= 3);

    // With a LUT after it (inverting 3D LUT), against the reference
    ColorGrade::Lut lut;
    lut.size = 9;
    lut.data.resize(9 * 9 * 9 * 3);
    for (int b = 0; b < 9; ++b)
        for (int g = 0; g < 9; ++g)
            for (int rr = 0; rr < 9; ++rr) {
                float* o = lut.data.data() + ((b * 9 + g) * 9 + rr) * 3;
                o[0] = 1 - rr / 8.0f;
                o[1] = 1 - g / 8.0f;
                o[2] = 1 - b / 8.0f;
            }
    ColorGrade::prepare(lut);
    compareBaked("qualifier + LUT", q, &lut, 12, 0.8);
    // Cached: the same correction gives the same tables
    CHECK(ColorGrade::bake(q, &lut) == ColorGrade::bake(q, &lut));
    CHECK(ColorGrade::bake(q, nullptr) != ColorGrade::bake(q, &lut));
}

Clip videoClip(int id)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.mp4";
    c.out = 99;
    return c;
}

void testModel(const QString& dir)
{
    Clip c = videoClip(1);
    EffectRegistry::add(c, "grade");
    const EffectInstance* e = EffectRegistry::instance(c, "grade");
    CHECK(!e->params.contains("curveY")); // sparse: a new grade stays small
    CHECK(!e->params.contains("q.hue"));
    CHECK(!ColorGrade::active(c));
    GradeQualifier::add(c);
    CHECK(GradeQualifier::exists(c));
    CHECK(!ColorGrade::active(c)); // a neutral qualifier changes nothing (the Renderer still shows its mask)
    CHECK(ColorGrade::at(c, 0).qualifier.on);
    CHECK(ColorGrade::at(c, 0).isNeutral());
    Keys::setValue(c, GradeQualifier::param("saturation"), 0, 20.0);
    Keys::setKey(c, GradeQualifier::param("hue"), 0, 10);
    Keys::setKey(c, GradeQualifier::param("hue"), 10, 30);
    CHECK_EQ(ColorGrade::at(c, 5).qualifier.range.hue, 20.0);
    CHECK_EQ(ColorGrade::at(c, 5).qualifier.correction.saturation, 20.0);
    CHECK_EQ(ColorGrade::at(c, 5).saturation, 50.0); // the grade's own stays
    GradeQualifier::remove(c);
    CHECK(!GradeQualifier::exists(c));
    CHECK(!Keys::hasKeys(c));

    // Editor with undo
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addMediaAt({"/x/a.mp4"}, 100, 0);
    QUndoStack& undo = *p.undoStack();
    const int id1 = p.timeline().video[0].clips[0].id, id2 = p.timeline().video[0].clips[1].id;
    const auto clip = [&](int id) { return TimelineOps::findClip(p.timeline(), id); };
    // A curve drag: many changes, one undo step
    const int before = undo.count();
    ed.setGradeCurve({id1}, K::Y, {{0, 0}, {0.5, 0.6}, {1, 1}}, "drag");
    ed.setGradeCurve({id1}, K::Y, {{0, 0}, {0.5, 0.7}, {1, 1}}, "drag");
    p.closeMerge();
    CHECK_EQ(undo.count(), before + 1);
    CHECK_EQ(ColorGrade::at(*clip(id1), 0).curves.points[int(K::Y)].value(1), QPointF(0.5, 0.7));
    ed.setGradeCurve({id1}, K::Y, {{0, 0}, {1, 1}}); // straight again: the key goes
    CHECK(!EffectRegistry::instance(*clip(id1), "grade")->params.contains("curveY"));
    undo.undo();
    CHECK(EffectRegistry::instance(*clip(id1), "grade")->params.contains("curveY"));

    ed.addGradeQualifier({id1});
    CHECK(GradeQualifier::exists(*clip(id1)));
    // Values of the qualifier only where it exists
    ed.setGradeValues({id1, id2}, {{GradeQualifier::param("saturation"), 0.0}}, 0, "Q");
    CHECK_EQ(ColorGrade::at(*clip(id1), 0).qualifier.correction.saturation, 0.0);
    CHECK(!EffectRegistry::has(*clip(id2), "grade")
          || !EffectRegistry::instance(*clip(id2), "grade")->params.contains("q.saturation"));
    GradeQualifier::Range r = GradeQualifier::around(QColor(40, 90, 200));
    r.invert = true;
    ed.setGradeQualifierRange({id1}, r, 0);
    CHECK(GradeQualifier::rangeAt(*clip(id1), 0) == r);
    // Invert on several clips keeps each clip's own range and adds no keyframes (review 0.24)
    {
        ed.addGradeQualifier({id2});
        const GradeQualifier::Range r2 = GradeQualifier::rangeAt(*clip(id2), 0);
        ed.setGradeQualifierInvert({id1, id2}, false);
        GradeQualifier::Range want1 = r, want2 = r2;
        want1.invert = false;
        CHECK(GradeQualifier::rangeAt(*clip(id1), 0) == want1);
        CHECK(GradeQualifier::rangeAt(*clip(id2), 0) == want2);
        CHECK(!Keys::animated(*clip(id1), GradeQualifier::param("hue")));
        ed.setGradeQualifierInvert({id1}, true);
        undo.undo(); // invert
        undo.undo(); // invert
        undo.undo(); // add on clip 2
        CHECK(!GradeQualifier::exists(*clip(id2)));
        CHECK(GradeQualifier::rangeAt(*clip(id1), 0) == r);
    }
    // Low end above the high one: read as the range between them
    {
        Clip x = videoClip(9);
        EffectRegistry::add(x, "grade");
        GradeQualifier::add(x);
        EffectRegistry::instance(x, "grade")->params["q.satLow"] = 70.0;
        EffectRegistry::instance(x, "grade")->params["q.satHigh"] = 20.0;
        const GradeQualifier::Range q = GradeQualifier::rangeAt(x, 0);
        CHECK(q.satLow == 20.0 && q.satHigh == 70.0);
        // A neutral qualifier changes nothing: the grade is not active for it (the mask still shows, testRender)
        CHECK(!ColorGrade::active(x));
    }
    // Keyframe of the qualifier layer: its values, not the grade's
    ed.setGradeKeyframe({id1}, 20, true, GradeQualifier::kLayer);
    CHECK(Keys::keyAt(*clip(id1), GradeQualifier::param("hue"), 20));
    CHECK(Keys::keyAt(*clip(id1), GradeQualifier::param("liftY"), 20));
    CHECK(!Keys::keyAt(*clip(id1), AnimParam::GradeLiftY, 20));
    // Copy grade takes curves and qualifier along
    CHECK(ed.copyGrade(id1));
    ed.pasteGrade({id2});
    CHECK(GradeQualifier::exists(*clip(id2)));
    CHECK(ColorGrade::at(*clip(id2), 0).curves == ColorGrade::at(*clip(id1), 0).curves);

    // Project file round trip
    const QString path = dir + "/q.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData d;
    CHECK(ProjectFile::load(path, &d, &error));
    const Clip* l1 = TimelineOps::findClip(d.timeline, id1);
    if (CHECK(l1)) {
        CHECK(*l1 == *clip(id1));
        CHECK(ColorGrade::at(*l1, 20) == ColorGrade::at(*clip(id1), 20));
    }
    ed.removeGradeQualifier({id1});
    CHECK(!GradeQualifier::exists(*clip(id1)));
    undo.undo();
    CHECK(GradeQualifier::exists(*clip(id1)));
}

void testReviewModel()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    const auto clip = [&] { return TimelineOps::findClip(p.timeline(), id); };
    ed.addGradeWindow({id}, 1, "ellipse");
    ed.addGradeQualifier({id});
    ed.setGradeValues({id}, {{GradeQualifier::param("saturation"), 10.0}}, 0, "q");
    ed.setGradeQualifierWindow({id}, 1);
    CHECK_EQ(GradeQualifier::windowOf(*clip()), 1);
    // Thumbnails know the qualifier's window (a change of it is a change of the picture)
    const GradedThumbs::Spec inWindow = GradedThumbs::specAt(*clip(), 0);
    CHECK(inWindow.qualifierWindow.has_value());
    ed.setGradeValues({id}, {{GradeWindows::param(1, "x"), 30.0}}, 0, "move");
    CHECK(GradedThumbs::specAt(*clip(), 0).hash != inWindow.hash);
    // Deleting the window frees the qualifier: a later window 1 does not limit it
    ed.removeGradeWindow({id}, 1);
    CHECK(!EffectRegistry::instance(*clip(), "grade")->params.contains("q.window"));
    ed.addGradeWindow({id}, 1, "rectangle");
    CHECK_EQ(GradeQualifier::windowOf(*clip()), 0);
    p.undoStack()->undo();
    p.undoStack()->undo();
    CHECK_EQ(GradeQualifier::windowOf(*clip()), 1); // one step: window and limit back together
    // Histogram read in place: the same for RGBA8888 and RGB32
    QImage rgb(321, 177, QImage::Format_RGB32);
    for (int y = 0; y < rgb.height(); ++y)
        for (int x = 0; x < rgb.width(); ++x) rgb.setPixel(x, y, qRgb(x % 256, (y * 3) % 256, (x + y) % 256));
    CHECK(CurveEditor::histograms(rgb) == CurveEditor::histograms(rgb.convertToFormat(QImage::Format_RGBA8888)));
}

void testPanel()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    const auto clip = [&] { return TimelineOps::findClip(p.timeline(), id); };
    ColorPanel panel(&ed);
    panel.resize(1000, 320);
    panel.setPlayhead(10);
    panel.show();
    QSignalSpy mask(&panel, &ColorPanel::qualifierMaskChanged);

    panel.setPalette(ColorPanel::CurvesPalette);
    CHECK_EQ(panel.palette(), int(ColorPanel::CurvesPalette));
    auto* curve = panel.findChild<CurveEditor*>("color_curve");
    if (CHECK(curve && curve->isVisible())) {
        QApplication::processEvents();
        // Click on the middle of the straight line, drag up: one undo step, the curve bends there
        const int before = p.undoStack()->count();
        const QPointF mid = curve->toWidget({0.5, 0.5});
        QMouseEvent press(QEvent::MouseButtonPress, mid, curve->mapToGlobal(mid), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(curve, &press);
        for (int i = 1; i <= 5; ++i) {
            const QPointF at = mid - QPointF(0, i * curve->area().height() * 0.04);
            QMouseEvent move(QEvent::MouseMove, at, curve->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, {});
            QApplication::sendEvent(curve, &move);
        }
        const QPointF end = mid - QPointF(0, curve->area().height() * 0.2);
        QMouseEvent release(QEvent::MouseButtonRelease, end, curve->mapToGlobal(end), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(curve, &release);
        CHECK_EQ(p.undoStack()->count(), before + 1);
        const auto& pts = ColorGrade::at(*clip(), 10).curves.points[int(K::Y)];
        CHECK_EQ(pts.size(), 3);
        CHECK(std::abs(GradeCurves::eval(pts, K::Y, 0.5) - 0.7) < 0.02);
        // Double-click on the point removes it: straight again
        const QPointF handle = curve->toWidget(pts.value(1));
        QMouseEvent dbl(QEvent::MouseButtonDblClick, handle, curve->mapToGlobal(handle), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(curve, &dbl);
        CHECK(ColorGrade::at(*clip(), 10).curves.isIdentity());
    }

    // Qualifier: the pick button asks for a pick but adds nothing yet (a cancelled pick leaves no qualifier)
    panel.setPalette(ColorPanel::QualifierPalette);
    {
        QSignalSpy pick(&panel, &ColorPanel::qualifierPickRequested);
        auto* button = panel.findChild<QPushButton*>("color_qualifier_pick");
        if (CHECK(button)) button->click();
        CHECK_EQ(pick.size(), 1);
        CHECK(!GradeQualifier::exists(*clip()));
    }
    // Picked colour, its layer, the mask follows the switch and the page
    panel.applyQualifierColor(QColor(200, 140, 110));
    CHECK(GradeQualifier::exists(*clip()));
    CHECK_EQ(panel.palette(), int(ColorPanel::QualifierPalette));
    CHECK(GradeQualifier::rangeAt(*clip(), 10) == GradeQualifier::around(QColor(200, 140, 110)));
    auto* maskButton = panel.findChild<QToolButton*>("color_qualifier_mask");
    if (CHECK(maskButton)) {
        maskButton->click();
        CHECK(!mask.isEmpty() && mask.last().value(0).toInt() == id);
        panel.hide();
        CHECK_EQ(mask.last().value(0).toInt(), 0);
        panel.show();
        CHECK_EQ(mask.last().value(0).toInt(), id);
        maskButton->click();
        CHECK_EQ(mask.last().value(0).toInt(), 0);
    }
    // GRADE_QUALIFIER_SHOTS=<dir>: pictures of the three palettes (a look at the layout)
    if (const QString shots = qEnvironmentVariable("GRADE_QUALIFIER_SHOTS"); !shots.isEmpty()) {
        ed.setGradeCurve({id}, K::Y, {{0, 0.05}, {0.3, 0.25}, {0.7, 0.8}, {1, 1}});
        ed.setGradeValues({id}, {{AnimParam::GradeGainY, 1.15}}, 10, "x");
        panel.resize(1300, 330);
        for (int i = 0; i < 3; ++i) {
            panel.setPalette(i);
            QApplication::processEvents();
            panel.grab().save(QString("%1/palette-%2.png").arg(shots).arg(i));
        }
    }
    panel.setLayer(GradeQualifier::kLayer);
    CHECK_EQ(panel.layer(), GradeQualifier::kLayer);
    ed.removeGradeQualifier({id});
    CHECK_EQ(panel.layer(), 0);
    panel.setLayer(GradeQualifier::kLayer); // gone: whole picture
    CHECK_EQ(panel.layer(), 0);
}

// The page fits its width (no sideways scrolling): wheels 2 x 2 on a narrow page, button rows wrap; also with a large
// font (the user tests with one). Widths of a 1280 px window beside the scopes and of a narrow one
void testFit()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addGradeQualifier({p.timeline().video[0].clips[0].id});
    const QFont normal = QApplication::font();
    for (const double pt : {normal.pointSizeF(), normal.pointSizeF() * 1.5}) {
        QFont f = normal;
        f.setPointSizeF(pt);
        QApplication::setFont(f);
        ColorPanel panel(&ed);
        panel.setPlayhead(10);
        panel.show();
        auto* scroll = panel.findChild<QScrollArea*>();
        if (!CHECK(scroll)) break;
        for (const int w : {1400, 900, 760}) {
            panel.resize(w, 320);
            for (int pal = 0; pal <= ColorPanel::StillsPalette; ++pal) {
                panel.setPalette(pal, false);
                QApplication::processEvents();
                const int need = scroll->widget()->minimumSizeHint().width();
                if (!CHECK(need <= scroll->viewport()->width())) {
                    std::printf("  font %.1f pt, %d px, palette %d: needs %d px of %d\n", pt, w, pal, need,
                                scroll->viewport()->width());
                    if (auto* st = panel.findChild<QStackedWidget*>())
                        for (int i = 0; i < st->count(); ++i)
                            std::printf("    page %d min %d\n", i, st->widget(i)->minimumSizeHint().width());
                    for (QWidget* c : scroll->widget()->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
                        std::printf("    %s min %d\n", c->metaObject()->className(), c->minimumSizeHint().width());
                }
            }
        }
    }
    QApplication::setFont(normal);
}

// Stills: a still keeps the grade it was taken with; its grade onto other clips (one undo step); stored in the project;
// compare = the still in the viewer overlay left of a wipe line that can be dragged
void testStills(const QString& dir)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addMediaAt({"/x/a.mp4"}, 100, 0);
    const int id1 = p.timeline().video[0].clips[0].id, id2 = p.timeline().video[0].clips[1].id;
    const auto clip = [&](int id) { return TimelineOps::findClip(p.timeline(), id); };
    ed.setGradeValues({id1}, {{AnimParam::GradeSaturation, 20.0}}, 0, "x");
    ed.setGradeCurve({id1}, K::Y, {{0, 0.1}, {1, 0.9}});
    ed.addGradeQualifier({id1});
    QImage picture(1280, 720, QImage::Format_RGB32);
    picture.fill(QColor(200, 120, 90));
    const int undoBefore = p.undoStack()->count();
    const int sid = ed.addGradeStill(id1, picture, "a.mp4 00:00:00:00");
    CHECK(sid > 0);
    CHECK_EQ(p.undoStack()->count(), undoBefore + 1);
    if (!CHECK_EQ(p.stills().size(), 1)) return;
    const GradeStill& st = p.stills().first();
    CHECK_EQ(GradeStills::picture(st).width(), GradeStills::kMaxEdge); // made small: the longest edge
    CHECK(st.image.size() < 60000);
    CHECK_EQ(st.grade.effects.size(), 1);
    // Later changes of the clip do not change the still
    ed.setGradeValues({id1}, {{AnimParam::GradeSaturation, 80.0}}, 0, "x");
    CHECK_EQ(ColorGrade::at(p.stills().first().grade, 0).saturation, 20.0);
    // Its grade onto clip 2: one step, curves and qualifier along
    const int before = p.undoStack()->count();
    CHECK(ed.applyGradeStill(sid, {id2}));
    CHECK_EQ(p.undoStack()->count(), before + 1);
    CHECK_EQ(ColorGrade::at(*clip(id2), 0).saturation, 20.0);
    CHECK(GradeQualifier::exists(*clip(id2)));
    CHECK(!ColorGrade::at(*clip(id2), 0).curves.isIdentity());
    CHECK(!ed.applyGradeStill(sid + 99, {id2}));
    // Project file
    const QString path = dir + "/stills.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData d;
    CHECK(ProjectFile::load(path, &d, &error));
    if (CHECK_EQ(d.stills.size(), 1)) {
        CHECK(d.stills.first() == p.stills().first());
        CHECK(!GradeStills::picture(d.stills.first()).isNull());
    }
    // Panel: the list, compare with a wipe line in the viewer overlay
    ColorPanel panel(&ed);
    panel.resize(1000, 320);
    panel.setPlayhead(10);
    panel.show();
    panel.setPalette(ColorPanel::StillsPalette);
    auto* list = panel.findChild<QListWidget*>("color_stills");
    if (CHECK(list)) CHECK_EQ(list->count(), 1);
    auto* compare = panel.findChild<QToolButton*>("color_still_compare");
    auto* ov = qobject_cast<WindowOverlay*>(panel.overlay());
    if (CHECK(compare && ov)) {
        compare->click();
        CHECK_EQ(panel.comparedStill(), sid);
        CHECK(ov->comparing());
        const QRectF frame(0, 0, 640, 360);
        // Drag the wipe line to a quarter
        const QPointF h = ov->wipeHandle(frame);
        QMouseEvent press(QEvent::MouseButtonPress, h, h, Qt::LeftButton, Qt::LeftButton, {});
        CHECK(ov->mousePress(&press, frame));
        const QPointF to(160, 180);
        QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, {});
        ov->mouseMove(&move, frame);
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, {});
        ov->mouseRelease(&release, frame);
        CHECK(std::abs(ov->wipe() - 0.25) < 1e-6);
        // Paints the still left of the line
        QImage canvas(640, 360, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        {
            QPainter painter(&canvas);
            ov->paint(painter, frame);
        }
        CHECK(canvas.pixelColor(40, 40).red() > 150);
        CHECK(canvas.pixelColor(400, 40) == QColor(Qt::black));
        // Side by side: the still whole in the left half, the live frame in the right one
        auto* side = panel.findChild<QToolButton*>("color_still_side");
        if (CHECK(side)) {
            side->click();
            CHECK(ov->sideBySide());
            QImage live(320, 180, QImage::Format_RGB32);
            live.fill(QColor(20, 200, 40));
            panel.setFrame(live);
            QImage both(640, 360, QImage::Format_RGB32);
            both.fill(Qt::white);
            {
                QPainter painter(&both);
                ov->paint(painter, frame);
            }
            CHECK(both.pixelColor(160, 180).red() > 150);  // still (orange) in the left half
            CHECK(both.pixelColor(480, 180).green() > 150); // live (green) in the right half
            CHECK(both.pixelColor(160, 20).red() < 40);    // letterboxed above it: the viewer's background
            side->click();
            CHECK(!ov->sideBySide());
        }
        // Removing the still ends the compare; undo brings it back
        ed.removeGradeStill(sid);
        CHECK(p.stills().isEmpty());
        CHECK_EQ(panel.comparedStill(), 0);
        CHECK(!ov->comparing());
        p.undoStack()->undo();
        CHECK_EQ(p.stills().size(), 1);
        if (list) CHECK_EQ(list->count(), 1);
    }
    if (const QString shots = qEnvironmentVariable("GRADE_QUALIFIER_SHOTS"); !shots.isEmpty()) {
        for (int i = 0; i < 3; ++i) ed.addGradeStill(i % 2 ? id2 : id1, picture, QString("a.mp4 00:00:0%1:00").arg(i));
        panel.resize(1300, 330);
        QApplication::processEvents();
        panel.grab().save(shots + "/palette-3.png");
    }
    // Release review 0.24: a portrait picture is limited by its longest edge; at most kMaxCount stills
    {
        QImage tall(1080, 1920, QImage::Format_RGB32);
        tall.fill(QColor(30, 90, 160));
        const GradeStill t = GradeStills::make(99, "tall", tall, *clip(id1));
        const QImage pic = GradeStills::picture(t);
        CHECK(pic.height() == GradeStills::kMaxEdge && pic.width() == 270);
        const int count = int(p.stills().size());
        for (int i = 0; i < GradeStills::kMaxCount + 2; ++i) ed.addGradeStill(id1, picture, "x");
        CHECK_EQ(int(p.stills().size()), GradeStills::kMaxCount);
        for (int i = 0; i < GradeStills::kMaxCount + 2; ++i) p.undoStack()->undo();
        CHECK_EQ(int(p.stills().size()), count);
    }
    // A still whose picture cannot be read: no compare with an empty picture
    {
        QVector<GradeStill> broken = p.stills();
        GradeStill bad = broken.first();
        bad.id = 50;
        bad.image = "not a picture";
        broken << bad;
        p.setStills(broken, "broken");
        panel.setComparedStill(50);
        CHECK_EQ(panel.comparedStill(), 0);
        CHECK(!ov->comparing());
        p.undoStack()->undo();
    }
    // The wipe fits a still of another shape (bars, not stretched)
    {
        QImage tall(360, 640, QImage::Format_RGB32);
        tall.fill(QColor(250, 250, 0));
        const int tallId = ed.addGradeStill(id1, tall, "tall");
        panel.setComparedStill(tallId);
        ov->setSideBySide(false);
        ov->setWipe(1.0);
        const QRectF frame(0, 0, 640, 360);
        QImage canvas(640, 360, QImage::Format_RGB32);
        canvas.fill(Qt::white);
        {
            QPainter painter(&canvas);
            ov->paint(painter, frame);
        }
        CHECK(canvas.pixelColor(320, 180).red() > 200 && canvas.pixelColor(320, 180).blue() < 60); // the still, centred
        CHECK(canvas.pixelColor(20, 180) == Theme::viewerBg); // bars beside it, not the still stretched
        panel.setComparedStill(0);
        p.undoStack()->undo();
    }
    // Grab only when the shown clip is under the playhead (picture and grade of one clip)
    {
        auto* grab = panel.findChild<QToolButton*>("color_still_grab");
        if (CHECK(grab)) {
            panel.setPlayhead(10);
            CHECK(grab->isEnabled());
            const int before = int(p.stills().size());
            panel.setPlayhead(5000); // past every clip: the earliest target is not under it
            panel.addStill(picture, "x");
            CHECK_EQ(int(p.stills().size()), before);
            panel.setPlayhead(10);
        }
    }
    // Another project: the compare ends
    {
        panel.setComparedStill(p.stills().first().id);
        CHECK(panel.comparedStill() != 0);
        ProjectData other = p.data();
        p.load(other);
        CHECK_EQ(panel.comparedStill(), 0);
    }
    // Apply from the panel onto the shown clip (clip 1 at the playhead)
    auto* apply = panel.findChild<QToolButton*>("color_still_apply");
    if (CHECK(apply)) {
        apply->click();
        CHECK_EQ(ColorGrade::at(*clip(id1), 0).saturation, 20.0);
    }
}

// CLI: color curves, qualifier op, info
QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString error(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

QJsonObject op(const char* json) { return QJsonDocument::fromJson(json).object(); }

void testCli(const QString& dir, const QString& still)
{
    const QString project = dir + "/cli.schneidi";
    run("new", {{"project", project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{still}}});
    run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", still}, {"duration", "2s"}}}}});
    ProjectData d;
    ProjectFile::load(project, &d, nullptr);
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int id = d.timeline.video[0].clips[0].id;
    const auto edit = [&](const QJsonObject& o) {
        QJsonObject x = o;
        x["clips"] = QJsonArray{id};
        return run("edit", {{"project", project}, {"ops", QJsonArray{x}}});
    };
    edit(op(R"({"op":"color","curves":{"y":[[0,0],[0.5,0.62],[1,1]],"hue_sat":[[0.55,0.5],[0.66,0.9],[0.8,0.5]]}})"));
    edit(op(R"({"op":"qualifier","color":"#d0302a","saturation":0,"hue_soft":12})"));
    ProjectFile::load(project, &d, nullptr);
    const Clip& c = d.timeline.video[0].clips[0];
    const CP p = ColorGrade::at(c, 0);
    CHECK_EQ(p.curves.points[int(K::Y)].size(), 3);
    CHECK_EQ(p.curves.points[int(K::HueSat)].size(), 3);
    CHECK(p.qualifier.on);
    CHECK_EQ(p.qualifier.correction.saturation, 0.0);
    CHECK_EQ(p.qualifier.range.hueSoft, 12.0);
    CHECK(std::abs(p.qualifier.range.hue - GradeQualifier::around(QColor("#d0302a")).hue) < 1e-6);
    // info shows the qualifier apart
    const QJsonObject info = run("info", {{"project", project}});
    CHECK(QJsonDocument(info).toJson().contains("\"qualifier\""));
    // Errors
    QJsonObject bad = op(R"({"op":"qualifier","sat_low":60,"sat_high":20})");
    bad["clips"] = QJsonArray{id};
    CHECK(error("edit", {{"project", project}, {"ops", QJsonArray{bad}}}).contains("sat_low is above sat_high"));
    bad = op(R"({"op":"color","curves":{"x":[[0,0]]}})");
    bad["clips"] = QJsonArray{id};
    CHECK(error("edit", {{"project", project}, {"ops", QJsonArray{bad}}}).contains("unknown curve"));
    bad = op(R"({"op":"color","curves":{"y":[[0]]}})");
    bad["clips"] = QJsonArray{id};
    CHECK(error("edit", {{"project", project}, {"ops", QJsonArray{bad}}}).contains("[x, y]"));
    edit(op(R"({"op":"color","curves":{"y":[]}})"));
    edit(op(R"({"op":"qualifier","remove":true})"));
    ProjectFile::load(project, &d, nullptr);
    CHECK(GradeCurves::isIdentity(ColorGrade::at(d.timeline.video[0].clips[0], 0).curves.points[int(K::Y)], K::Y));
    CHECK(!GradeQualifier::exists(d.timeline.video[0].clips[0]));

    // Stills: add (notes give the id), list in info, look at it with frames (alone and beside the timeline), apply,
    // remove; the qualifier limited to a window
    edit(op(R"({"op":"color","saturation":0})"));
    const QJsonObject added = edit(op(R"({"op":"still","add":true,"at":5,"label":"gray"})"));
    CHECK(QJsonDocument(added).toJson().contains("still 1 added"));
    const QJsonObject info2 = run("info", {{"project", project}});
    CHECK(info2["stills"].toArray().size() == 1 && info2["stills"].toArray()[0].toObject()["label"] == "gray");
    const QString out = dir + "/stillframes";
    QDir().mkpath(out);
    const QJsonObject alone = run("frames", {{"source", project}, {"still", 1}, {"out", out}, {"width", 320}});
    const QImage a = QImage(alone["frames"].toArray().first().toObject()["file"].toString());
    CHECK(!a.isNull() && a.width() == 320);
    if (!a.isNull()) CHECK(std::abs(a.pixelColor(160, 90).red() - a.pixelColor(160, 90).green()) < 12); // gray
    edit(op(R"({"op":"color","reset":true})"));
    const QJsonObject beside = run("frames", {{"source", project}, {"still", 1}, {"at", QJsonArray{5}}, {"out", out}, {"width", 640}});
    const QImage b = QImage(beside["frames"].toArray().first().toObject()["file"].toString());
    if (CHECK(!b.isNull() && b.width() > b.height() * 3)) { // two 16:9 pictures side by side
        const QColor l = b.pixelColor(b.width() / 4, b.height() / 2), r = b.pixelColor(b.width() * 3 / 4, b.height() / 2);
        CHECK(std::abs(l.red() - l.green()) < 12); // the still: gray figure
        CHECK(r.red() > 200 && r.green() < 60);    // the timeline now: red figure
    }
    { const QString e = error("frames", {{"source", project}, {"still", 9}, {"out", out}}); if (!CHECK(e.contains("no still 9"))) std::printf("  %s\n", qPrintable(e)); }
    CHECK(error("frames", {{"source", still}, {"still", 1}, {"at", QJsonArray{0}}, {"out", out}}).contains("still are for a project"));
    edit(op(R"({"op":"still","apply":1})"));
    ProjectFile::load(project, &d, nullptr);
    CHECK_EQ(ColorGrade::at(d.timeline.video[0].clips[0], 0).saturation, 0.0);
    CHECK(error("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "still"}, {"apply", 7}, {"clips", QJsonArray{id}}}}}})
              .contains("no still 7"));
    edit(op(R"({"op":"still","remove":1})"));
    ProjectFile::load(project, &d, nullptr);
    CHECK(d.stills.isEmpty());
    // Qualifier in window 1
    edit(op(R"({"op":"window","add":"rectangle"})"));
    edit(op(R"({"op":"qualifier","color":"#ff0000","saturation":0,"window":1})"));
    ProjectFile::load(project, &d, nullptr);
    CHECK_EQ(GradeQualifier::windowOf(d.timeline.video[0].clips[0]), 1);
    CHECK_EQ(ColorGrade::at(d.timeline.video[0].clips[0], 0).qualifier.window, 1);
    CHECK(error("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "qualifier"}, {"window", 5}, {"clips", QJsonArray{id}}}}}})
              .contains("no clip has window 5"));
    CHECK(error("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "qualifier"}, {"window", 1.5}, {"clips", QJsonArray{id}}}}}})
              .contains("window is 0"));
    edit(op(R"({"op":"qualifier","window":0})"));
    ProjectFile::load(project, &d, nullptr);
    CHECK_EQ(GradeQualifier::windowOf(d.timeline.video[0].clips[0]), 0);
}

// Through the picture engine: red figure on green, qualifier on red with saturation 0. Export: figure gray, green
// as it was. Preview with the mask switch on the clip: white figure on black; the export never shows the mask.
void testRender(const QString& still)
{
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 7;
    c.mediaPath = still;
    c.out = 49;
    EffectRegistry::add(c, "grade");
    GradeQualifier::add(c);
    EffectInstance* e = EffectRegistry::instance(c, "grade");
    const GradeQualifier::Range r = GradeQualifier::around(QColor(255, 0, 0));
    e->params["q.hue"] = r.hue;
    e->params["q.hueWidth"] = r.hueWidth;
    e->params["q.satLow"] = r.satLow;
    e->params["q.satHigh"] = r.satHigh;
    e->params["q.lumLow"] = 0.0;
    e->params["q.lumHigh"] = 100.0;
    e->params["q.saturation"] = 0.0;
    tl.video[0].clips << c;
    auto mask = std::make_shared<std::atomic<int>>(7);
    const auto render = [&](Picture::Mode mode) {
        Picture::Options o;
        o.format = ProjectFormat{320, 180, {25, 1}};
        o.mode = mode;
        o.gpu = false;
        o.qualifierMask = mask;
        Picture::Renderer rd(tl, o);
        return rd.render(2).toImage().convertToFormat(QImage::Format_RGB32);
    };
    const QImage out = render(Picture::Mode::Export);
    const QImage pre = render(Picture::Mode::Preview);
    if (!CHECK(!out.isNull() && !pre.isNull())) return;
    const QPoint figure(160, 90), back(20, 20);
    const QColor f = out.pixelColor(figure), b = out.pixelColor(back);
    CHECK(std::abs(f.red() - f.green()) < 12);  // gray
    CHECK(b.green() > 200 && b.red() < 30);     // green untouched
    CHECK(qGray(pre.pixel(figure)) > 240);       // mask: white
    CHECK(qGray(pre.pixel(back)) < 15);          // black
    mask->store(0);
    const QImage plain = render(Picture::Mode::Preview);
    CHECK(std::abs(plain.pixelColor(figure).red() - plain.pixelColor(figure).green()) < 12);
    // A neutral qualifier: the grade is not active, the mask still shows in the preview
    tl.video[0].clips[0].effects[0].params["q.saturation"] = 50.0;
    CHECK(!ColorGrade::active(tl.video[0].clips[0]));
    mask->store(7);
    const QImage neutral = render(Picture::Mode::Preview);
    CHECK(qGray(neutral.pixel(figure)) > 240 && qGray(neutral.pixel(back)) < 15);
    CHECK_EQ(render(Picture::Mode::Export).pixelColor(back), b);
    // Limited to a window over the left half of the figure: only that half gets gray; the mask is black outside it
    {
        Clip& q = tl.video[0].clips[0];
        q.effects[0].params["q.saturation"] = 0.0;
        GradeWindows::add(q, 1, GradeWindows::Shape::Rectangle);
        EffectInstance& g = q.effects[0];
        g.params["w1.x"] = 25.0;
        g.params["w1.width"] = 50.0;
        g.params["w1.height"] = 100.0;
        g.params["w1.softness"] = 0.0;
        g.params["q.window"] = 1.0;
        CHECK_EQ(GradeQualifier::windowOf(q), 1);
        mask->store(0);
        const QImage half = render(Picture::Mode::Export);
        const QColor leftFig = half.pixelColor(130, 90), rightFig = half.pixelColor(190, 90);
        CHECK(std::abs(leftFig.red() - leftFig.green()) < 12); // inside the window: gray
        CHECK(rightFig.red() > 200 && rightFig.green() < 60);  // outside: still red
        mask->store(7);
        const QImage m = render(Picture::Mode::Preview);
        CHECK(qGray(m.pixel(130, 90)) > 240);
        CHECK(qGray(m.pixel(190, 90)) < 15);
        mask->store(0);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("grade-qualifier");
    if (qEnvironmentVariableIsSet("GRADE_QUALIFIER_SHOTS")) Theme::apply(app); // the pictures in the app's own look
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testCurves();
    testQualifierMath();
    testPixels();
    testModel(tmp.path());
    testReviewModel();
    testPanel();
    testStills(tmp.path());
    testFit();

    Check::initEngine();
    const QString still = tmp.filePath("figure.png");
    {
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(QColor(0, 255, 0));
        for (int y = 60; y < 120; ++y)
            for (int x = 120; x < 200; ++x) img.setPixel(x, y, qRgb(255, 0, 0));
        CHECK(img.save(still));
    }
    testCli(tmp.path(), still);
    testRender(still);
    return Check::result();
}
