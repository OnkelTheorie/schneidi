// SPDX-License-Identifier: AGPL-3.0-only
// A whole clip tilted in depth (0.20, Inspector Transform "Kippen X/Y", "Perspektive"):
// - project file: the values and their keyframes survive save/load; a clip without them stays without them (old
//   files: no keys = flat, fov 40)
// - keyframes: TiltY animates between two keys
// - CPU render of a tilted clip = a trapezoid (the side that goes away is shorter), tilt X likewise top/bottom
// - tilt 0 (also keyframes that stay at 0, another field of view) = bit for bit the flat picture
// - preview and export draw the same tilted picture (the GPU path hands tilted clips to the CPU: test_picture_gpu_render)
// - CLI: op 'transform' tilt_x/tilt_y/fov, op 'keyframe' param tilt_y, `info` shows them, values kept in range
#include "render_check.h"

#include "cli/Commands.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"

#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <clocale>
#include <cmath>

namespace {

const ProjectFormat kFormat{320, 180, {25, 1}};
QString g_still;

Clip still(int id = 1)
{
    Clip c;
    c.id = id;
    c.mediaPath = g_still;
    c.start = 0;
    c.in = 0;
    c.out = 49;
    c.transform.zoomX = c.transform.zoomY = 0.6;
    return c;
}

Timeline oneTrack(const Clip& c)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    return tl;
}

QImage render(const Clip& c, int frame, Picture::Mode mode = Picture::Mode::Export)
{
    Picture::Options o;
    o.format = kFormat;
    o.mode = mode;
    o.transparent = true;
    Picture::Renderer r(oneTrack(c), o);
    return r.render(frame).toImage().convertToFormat(QImage::Format_RGBA8888);
}

// Opaque pixels in column x / row y
int column(const QImage& img, int x)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y) n += qAlpha(img.pixel(x, y)) > 128;
    return n;
}
int row(const QImage& img, int y)
{
    int n = 0;
    for (int x = 0; x < img.width(); ++x) n += qAlpha(img.pixel(x, y)) > 128;
    return n;
}
// First and last column (row) with anything opaque
std::pair<int, int> spanX(const QImage& img)
{
    int l = 0, r = img.width() - 1;
    while (l < img.width() && column(img, l) == 0) ++l;
    while (r > 0 && column(img, r) == 0) --r;
    return {l, r};
}
std::pair<int, int> spanY(const QImage& img)
{
    int t = 0, b = img.height() - 1;
    while (t < img.height() && row(img, t) == 0) ++t;
    while (b > 0 && row(img, b) == 0) --b;
    return {t, b};
}

int maxDiff(const QImage& a, const QImage& b)
{
    if (a.isNull() || a.size() != b.size()) return 999;
    int m = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uint8_t* p = a.constScanLine(y);
        const uint8_t* q = b.constScanLine(y);
        for (int i = 0; i < a.width() * 4; ++i) m = std::max(m, std::abs(int(p[i]) - int(q[i])));
    }
    return m;
}

void testProjectFile(const QString& dir)
{
    ProjectData d;
    d.format = kFormat;
    Clip c = still();
    c.transform.tiltX = 25;
    c.transform.tiltY = -12.5;
    c.transform.fov = 70;
    Keys::setKey(c, AnimParam::TiltY, 0, -12.5);
    Keys::setKey(c, AnimParam::TiltY, 20, 30);
    Keys::setKey(c, AnimParam::Fov, 10, 55);
    Clip flat = still(2);
    flat.start = 50;
    d.timeline = oneTrack(c);
    d.timeline.video[0].clips << flat;
    const QString path = dir + "/tilt.schneidi";
    QString error;
    if (!CHECK(ProjectFile::save(d, path, &error))) return;
    ProjectData back;
    if (!CHECK(ProjectFile::load(path, &back, &error))) return;
    if (!CHECK(back.timeline.video.size() == 1 && back.timeline.video[0].clips.size() == 2)) return;
    const Clip& a = back.timeline.video[0].clips[0];
    CHECK(a.transform.tiltX == 25 && a.transform.tiltY == -12.5 && a.transform.fov == 70);
    CHECK(a.keys.value(AnimParam::TiltY) == c.keys.value(AnimParam::TiltY));
    CHECK(a.keys.value(AnimParam::Fov) == c.keys.value(AnimParam::Fov));
    const Clip& b = back.timeline.video[0].clips[1];
    CHECK(b.transform.tiltX == 0 && b.transform.tiltY == 0 && b.transform.fov == 40);
    CHECK(!b.transform.hasTilt() && !Keys::hasTilt(b));
    // Flat clips write nothing new (older versions read the file as before)
    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray text = f.readAll();
    CHECK(text.count("\"tiltX\"") == 1 && text.count("\"tiltY\"") == 2); // the value and its keyframes
}

void testModel()
{
    Clip c = still();
    CHECK(!Keys::hasTilt(c));
    c.transform.fov = 90; // a field of view alone changes nothing
    CHECK(!Keys::hasTilt(c));
    c.transform.tiltX = 10;
    CHECK(Keys::hasTilt(c) && c.transform.hasTransform());
    c.transform.transformOn = false; // the section switched off: flat
    CHECK(!Keys::hasTilt(c) && !Keys::hasTransform(c));
    // Keyframes animate it (and make a clip without static tilt a transformed one)
    Clip k = still();
    k.transform.zoomX = k.transform.zoomY = 1;
    CHECK(!Keys::hasTransform(k));
    Keys::setKey(k, AnimParam::TiltY, 0, 0);
    Keys::setKey(k, AnimParam::TiltY, 10, 60);
    CHECK(Keys::hasTilt(k) && Keys::hasTransform(k));
    CHECK(std::abs(Keys::valueAt(k, AnimParam::TiltY, 5) - 30) < 1e-9);
    AnimParam p;
    CHECK(Keys::fromId("tiltX", &p) && p == AnimParam::TiltX);
    CHECK(Keys::fromId("fov", &p) && p == AnimParam::Fov);
    double lo = 0, hi = 0;
    Keys::range(AnimParam::TiltY, &lo, &hi);
    CHECK(lo == -89 && hi == 89);
}

void testRender()
{
    // Flat: a centered rectangle, left and right sides equal
    const QImage flat = render(still(), 3);
    {
        const auto [l, r] = spanX(flat);
        CHECK(r > l + 100);
        CHECK(std::abs(column(flat, l + 2) - column(flat, r - 2)) <= 1);
    }
    // Tilt Y +40: the right side goes away -> shorter than the left one; the picture keeps its center
    Clip ty = still();
    ty.transform.tiltY = 40;
    const QImage y = render(ty, 3);
    {
        const auto [l, r] = spanX(y);
        CHECK(r > l + 50);
        const int left = column(y, l + 2), right = column(y, r - 2);
        std::printf("  tilt Y 40: columns %d..%d, left %d px high, right %d\n", l, r, left, right);
        CHECK(right + 10 < left);
        CHECK(qAlpha(y.pixel(160, 90)) == 255); // the middle stays covered
    }
    // Negative: the left side goes away
    ty.transform.tiltY = -40;
    {
        const QImage n = render(ty, 3);
        const auto [l, r] = spanX(n);
        CHECK(column(n, l + 2) + 10 < column(n, r - 2));
    }
    // Tilt X +40: the top goes away -> narrower than the bottom
    Clip tx = still();
    tx.transform.tiltX = 40;
    {
        const QImage x = render(tx, 3);
        const auto [t, b] = spanY(x);
        CHECK(b > t + 30);
        std::printf("  tilt X 40: rows %d..%d, top %d px wide, bottom %d\n", t, b, row(x, t + 2), row(x, b - 2));
        CHECK(row(x, t + 2) + 10 < row(x, b - 2));
    }
    // A wider field of view: stronger perspective (the far side shorter still)
    {
        Clip wide = still();
        wide.transform.tiltY = 40;
        wide.transform.fov = 100;
        const QImage w = render(wide, 3);
        const auto [l, r] = spanX(w);
        const auto [l0, r0] = spanX(y);
        CHECK(double(column(w, r - 2)) / column(w, l + 2) < double(column(y, r0 - 2)) / column(y, l0 + 2));
    }
    // Keyframed: flat at frame 0, half way at 5, most at 10
    {
        Clip k = still();
        Keys::setKey(k, AnimParam::TiltY, 0, 0);
        Keys::setKey(k, AnimParam::TiltY, 10, 60);
        const QImage f0 = render(k, 0), f5 = render(k, 5), f10 = render(k, 10);
        CHECK(maxDiff(f0, flat) == 0);
        auto ratio = [](const QImage& img) {
            const auto [l, r] = spanX(img);
            return double(column(img, r - 2)) / std::max(1, column(img, l + 2));
        };
        CHECK(ratio(f10) < ratio(f5) && ratio(f5) < 0.98);
    }
    // Tilt 0 = bit for bit the flat picture (another fov, keyframes that stay at 0, rotation and position too)
    {
        Clip a = still();
        a.transform.rotation = 12;
        a.transform.posX = 30;
        Clip b = a;
        b.transform.fov = 95;
        Keys::setKey(b, AnimParam::TiltX, 0, 0);
        Keys::setKey(b, AnimParam::TiltX, 20, 0);
        CHECK(Keys::hasTilt(b));
        CHECK(maxDiff(render(a, 4), render(b, 4)) == 0);
        CHECK(maxDiff(render(a, 4, Picture::Mode::Preview), render(b, 4, Picture::Mode::Preview)) == 0);
    }
    // Preview draws what the export writes
    {
        Clip t = still();
        t.transform.tiltX = -25;
        t.transform.tiltY = 30;
        t.transform.rotation = 8;
        CHECK(maxDiff(render(t, 2), render(t, 2, Picture::Mode::Preview)) <= 2);
    }
    // Behind the camera (a corner past it at a wide angle, zoomed far in): nothing drawn, no crash
    {
        Clip t = still();
        t.transform.zoomX = t.transform.zoomY = 8;
        t.transform.tiltY = 89;
        t.transform.fov = 120;
        const QImage img = render(t, 1);
        CHECK(!img.isNull());
    }
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        Check::report(false, qPrintable(e.code + ": " + e.message), __FILE__, __LINE__);
        return {};
    }
}

void testCli(const QString& dir)
{
    const QString path = dir + "/cli.schneidi";
    {
        Project p;
        ProjectData d;
        d.format = kFormat;
        d.timeline = emptyTimeline();
        p.load(d);
        p.edit("setup", [&](Timeline& tl) {
            Clip v = still(p.newClipId());
            v.transform = ClipTransform{};
            tl.video[0].clips << v;
        });
        QString error;
        if (!CHECK(ProjectFile::save(p.data(), path, &error))) return;
    }
    const int id = [&] {
        ProjectData d;
        QString error;
        ProjectFile::load(path, &d, &error);
        return d.timeline.video.value(0).clips.value(0).id;
    }();
    run("edit", {{"project", path},
                 {"ops", QString("[{\"op\":\"transform\",\"clips\":[%1],\"tilt_x\":30,\"tilt_y\":-20,\"fov\":60},"
                                 "{\"op\":\"keyframe\",\"clips\":[%1],\"param\":\"tilt_y\",\"at\":10,\"value\":45}]")
                             .arg(id)}});
    ProjectData d;
    QString error;
    if (!CHECK(ProjectFile::load(path, &d, &error))) return;
    const Clip* c = TimelineOps::findClip(d.timeline, id);
    if (!CHECK(c)) return;
    CHECK(c->transform.tiltX == 30 && c->transform.fov == 60);
    CHECK(Keys::animated(*c, AnimParam::TiltY) && std::abs(Keys::valueAt(*c, AnimParam::TiltY, 10) - 45) < 1e-9);
    // Out of range: kept inside -89..89 and 5..120
    run("edit", {{"project", path},
                 {"ops", QString("[{\"op\":\"transform\",\"clips\":[%1],\"tilt_x\":200,\"fov\":1}]").arg(id)}});
    ProjectFile::load(path, &d, &error);
    c = TimelineOps::findClip(d.timeline, id);
    if (!CHECK(c)) return;
    CHECK(c->transform.tiltX == 89 && c->transform.fov == 5);
    // `info` shows the values and the keyframes by their op names
    const QJsonObject info = run("info", {{"project", path}});
    const QByteArray text = QJsonDocument(info).toJson(QJsonDocument::Compact);
    CHECK(text.contains("\"tilt_x\":89") && text.contains("\"fov\":5") && text.contains("\"tilt_y\":["));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("clip_tilt");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    QImage s(320, 180, QImage::Format_RGBA8888);
    s.fill(QColor(230, 200, 40));
    g_still = tmp.filePath("still.png");
    if (!CHECK(s.save(g_still))) return Check::result();
    Check::initEngine();
    testModel();
    testProjectFile(tmp.path());
    testRender();
    testCli(tmp.path());
    return Check::result();
}
