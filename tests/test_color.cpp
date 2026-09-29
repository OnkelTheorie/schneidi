// Test Color-Seite (Effekt "grade"): Pixelrechnung (Lift/Gamma/Gain/Offset, Kontrast, Sättigung, Temperatur,
// Belichtung, LUT), .cube-Leser, Render über MLT (Vorschau-Bypass, Keyframes, Teilen, deutsches Zahlenformat),
// Editor-Operationen mit Undo, Speichern/Laden mit LUT-Pfad (auch verschobener Ordner) und alte Dateien.
#include "check.h"

#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cstring>
#include <functional>

namespace {

QString g_media;

// Grauverlauf 0..255 in R=G=B (eine Zeile)
std::vector<uint8_t> ramp()
{
    std::vector<uint8_t> px(256 * 4);
    for (int i = 0; i < 256; ++i) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = uint8_t(i);
        px[i * 4 + 3] = 200;
    }
    return px;
}
std::vector<uint8_t> graded(const ColorGrade::Params& p, const ColorGrade::Lut* lut = nullptr)
{
    std::vector<uint8_t> px = ramp();
    ColorGrade::apply(px.data(), 256, 1, p, lut);
    return px;
}
int R(const std::vector<uint8_t>& px, int i) { return px[i * 4]; }
int G(const std::vector<uint8_t>& px, int i) { return px[i * 4 + 1]; }
int B(const std::vector<uint8_t>& px, int i) { return px[i * 4 + 2]; }

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

// .cube-Datei: fn bildet (r, g, b) 0..1 ab
QByteArray cube(int n, const std::function<void(double&, double&, double&)>& fn)
{
    QByteArray s = "# Test\nTITLE \"t\"\nLUT_3D_SIZE " + QByteArray::number(n) + "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                double x = r / double(n - 1), y = g / double(n - 1), z = b / double(n - 1);
                fn(x, y, z);
                s += QByteArray::number(x, 'f', 6) + " " + QByteArray::number(y, 'f', 6) + " "
                     + QByteArray::number(z, 'f', 6) + "\n";
            }
    return s;
}

QImage render(const Timeline& tl, const ProjectFormat& fmt, int pos, std::shared_ptr<std::atomic<bool>> bypass = {})
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    if (bypass) b.setGradeBypass(bypass);
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
    c.out = 99;
    fn(c);
    tl.video[0].clips << c;
    return tl;
}

std::function<void(Clip&)> grade(std::initializer_list<std::pair<const char*, QVariant>> vals)
{
    QVector<std::pair<const char*, QVariant>> v(vals);
    return [=](Clip& c) {
        EffectRegistry::add(c, "grade");
        for (auto [k, x] : v) EffectRegistry::instance(c, "grade")->params[k] = x;
    };
}

struct Rgb { double r = 0, g = 0, b = 0; };
Rgb mean(const QImage& img, QRectF rel)
{
    const QRect q(int(rel.x() * img.width()), int(rel.y() * img.height()), int(rel.width() * img.width()),
                  int(rel.height() * img.height()));
    Rgb s;
    int n = 0;
    for (int y = q.top(); y <= q.bottom(); ++y)
        for (int x = q.left(); x <= q.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            s.r += c.red(); s.g += c.green(); s.b += c.blue();
            ++n;
        }
    if (n) { s.r /= n; s.g /= n; s.b /= n; }
    return s;
}
Rgb gray(const QImage& img) { return mean(img, QRectF(0.02, 0.1, 0.08, 0.4)); } // SMPTE-Balken: 75 % grau
Rgb cyan(const QImage& img) { return mean(img, QRectF(0.35, 0.1, 0.05, 0.4)); }

double diff(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size()) return 1e9;
    double sum = 0;
    for (int y = 0; y < a.height(); y += 4) {
        const uchar* p = a.constScanLine(y);
        const uchar* q = b.constScanLine(y);
        for (int x = 0; x < a.width() * 4; ++x) sum += std::abs(p[x] - q[x]);
    }
    return sum / (a.width() * ((a.height() + 3) / 4) * 4.0);
}

void testPixels(const QString& dir)
{
    using CP = ColorGrade::Params;
    const std::vector<uint8_t> in = ramp();
    CHECK(CP().isNeutral());
    CHECK(graded(CP()) == in); // neutral = unverändert, Alpha bleibt

    CP p;
    p.lift[0] = 0.2; // Schwarz hoch, Weiß bleibt
    auto o = graded(p);
    CHECK(R(o, 0) > 40 && R(o, 255) == 255);
    p = CP();
    p.lift[0] = -0.1; // Lift darf negativ (Schwarz wird abgeschnitten)
    o = graded(p);
    CHECK(R(o, 20) == 0 && R(o, 255) == 255);
    p = CP();
    p.gain[0] = 0.5; // Weiß runter, Schwarz bleibt
    o = graded(p);
    CHECK(R(o, 0) == 0 && std::abs(R(o, 255) - 128) <= 1);
    p = CP();
    p.gamma[0] = 0.2; // Mitten heller, Enden bleiben
    o = graded(p);
    CHECK(R(o, 0) == 0 && R(o, 255) == 255 && R(o, 128) > 145);
    p = CP();
    p.offset[0] = 35; // alles +0,1
    o = graded(p);
    CHECK(std::abs(R(o, 100) - (100 + 26)) <= 1);
    p = CP();
    p.contrast = 1.5; // um den Pivot (0,435)
    o = graded(p);
    CHECK(R(o, 60) < 60 && R(o, 200) > 200 && std::abs(R(o, 111) - 111) <= 1);
    p = CP();
    p.exposure = 1; // +1 Blende
    o = graded(p);
    CHECK(R(o, 100) > 120 && R(o, 0) == 0);
    // Farbräder: R-Anteil nur im roten Kanal
    p = CP();
    p.gain[1] = 1.2;
    p.lift[3] = 0.1;
    o = graded(p);
    CHECK(R(o, 150) > 170 && G(o, 150) == 150 && B(o, 10) > 30 && G(o, 10) == 10);
    // Temperatur: warm = mehr Rot als Blau, Tönung + = weniger Grün
    p = CP();
    p.temperature = 2000;
    o = graded(p);
    CHECK(R(o, 128) > B(o, 128) + 20);
    p = CP();
    p.tint = 60;
    o = graded(p);
    CHECK(G(o, 128) < R(o, 128) - 10);
    // Sättigung 0 = grau
    {
        std::vector<uint8_t> px{200, 50, 30, 255};
        CP s;
        s.saturation = 0;
        ColorGrade::apply(px.data(), 1, 1, s, nullptr);
        CHECK(px[0] == px[1] && px[1] == px[2] && px[3] == 255);
        std::vector<uint8_t> px2{200, 50, 30, 255};
        s.saturation = 100;
        ColorGrade::apply(px2.data(), 1, 1, s, nullptr);
        CHECK(px2[0] > 200 && px2[1] < 50);
    }

    // LUT: Identität lässt alles, Invertieren kehrt um, Pfad mit Leerzeichen/Umlaut
    const QString idPath = QDir(dir).filePath("ident ität.cube");
    const QString invPath = QDir(dir).filePath("invert.cube");
    CHECK(writeFile(idPath, cube(17, [](double&, double&, double&) {})));
    CHECK(writeFile(invPath, cube(9, [](double& r, double& g, double& b) { r = 1 - r; g = 1 - g; b = 1 - b; })));
    QString err;
    auto idLut = ColorGrade::loadCube(idPath, &err);
    auto invLut = ColorGrade::loadCube(invPath, &err);
    if (CHECK(idLut && invLut)) {
        CHECK(idLut->size == 17 && idLut->is3d);
        o = graded(CP(), idLut.get());
        bool same = true;
        for (int i = 0; i < 256; ++i) same = same && std::abs(R(o, i) - i) <= 1;
        CHECK(same);
        o = graded(CP(), invLut.get());
        CHECK(R(o, 0) == 255 && R(o, 255) == 0 && std::abs(R(o, 100) - 155) <= 1);
        CHECK(o[3] == 200); // Alpha
        CHECK(ColorGrade::loadCube(idPath) == idLut); // zwischengespeichert
    }
    // 1D-LUT mit Domain und kaputte Dateien
    const QString oneD = QDir(dir).filePath("half.cube");
    CHECK(writeFile(oneD, "LUT_1D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n0 0 0\n0.5 0.5 0.5\n"));
    if (auto l = ColorGrade::loadCube(oneD); CHECK(l && !l->is3d)) CHECK(std::abs(R(graded(CP(), l.get()), 255) - 128) <= 1);
    const QString bad = QDir(dir).filePath("bad.cube");
    CHECK(writeFile(bad, "LUT_3D_SIZE 2\n0 0 0\n1 1 1\n"));
    err.clear();
    CHECK(!ColorGrade::loadCube(bad, &err) && !err.isEmpty());
    CHECK(!ColorGrade::loadCube(QDir(dir).filePath("fehlt.cube")));
}

void testRender(const QString& dir)
{
    const ProjectFormat fmt;
    const QImage ref = render(one([](Clip&) {}), fmt, 10);
    if (!CHECK(!ref.isNull())) return;
    const Rgb g0 = gray(ref);
    CHECK(g0.r > 150);

    // Neutral / nur angelegt / ausgeschaltet = unverändert
    CHECK(diff(render(one(grade({})), fmt, 10), ref) < 0.5);
    CHECK(diff(render(one([](Clip& c) {
                   grade({{"gainY", 0.5}})(c);
                   c.effects[0].enabled = false;
               }), fmt, 10), ref) < 0.5);
    // Gain halbiert das Grau, Sättigung 0 macht Cyan grau
    const Timeline darker = one(grade({{"gainY", 0.5}}));
    const QImage dark = render(darker, fmt, 10);
    CHECK(std::abs(gray(dark).r - g0.r / 2) < 6);
    const Rgb gcy = cyan(render(one(grade({{"saturation", 0.0}})), fmt, 10));
    CHECK(std::abs(gcy.r - gcy.g) < 6 && std::abs(gcy.g - gcy.b) < 6);

    // Vorschau-Schalter Vorher/Nachher: live umschaltbar, ohne Schalter (Export) immer korrigiert
    auto bypass = std::make_shared<std::atomic<bool>>(true);
    CHECK(diff(render(darker, fmt, 10, bypass), ref) < 0.5);
    bypass->store(false);
    CHECK(diff(render(darker, fmt, 10, bypass), dark) < 0.5);
    {
        auto prof = makeProfile(fmt);
        TimelineBuilder b(*prof);
        auto flag = std::make_shared<std::atomic<bool>>(false);
        b.setGradeBypass(flag);
        auto tr = b.build(darker);
        auto grab = [&] {
            tr->seek(10);
            std::unique_ptr<Mlt::Frame> f(tr->get_frame());
            mlt_image_format ifmt = mlt_image_rgba;
            int w = prof->width(), h = prof->height();
            const uint8_t* d = f->get_image(ifmt, w, h);
            QImage img(w, h, QImage::Format_RGBA8888);
            if (d) std::memcpy(img.bits(), d, size_t(w) * h * 4);
            return img;
        };
        CHECK(diff(grab(), dark) < 0.5);
        flag->store(true); // gleicher Tractor, kein Neuaufbau
        CHECK(diff(grab(), ref) < 0.5);
    }

    // LUT im Render (Invertieren)
    const QString inv = QDir(dir).filePath("render invert.cube");
    CHECK(writeFile(inv, cube(5, [](double& r, double& g, double& b) { r = 1 - r; g = 1 - g; b = 1 - b; })));
    const Rgb gi = gray(render(one(grade({{"lut", inv}})), fmt, 10));
    CHECK(std::abs(gi.r - (255 - g0.r)) < 6);
    // Fehlende LUT: Bild bleibt (kein Absturz)
    CHECK(diff(render(one(grade({{"lut", QDir(dir).filePath("weg.cube")}})), fmt, 10), ref) < 0.5);

    // Keyframes Gain 0 (Frame 0) -> 1 (Frame 50); Teilen bei 25 ändert nichts
    const Timeline keyed = one([](Clip& c) {
        grade({})(c);
        Keys::setKey(c, AnimParam::GradeGainY, 0, 0.0);
        Keys::setKey(c, AnimParam::GradeGainY, 50, 1.0);
    });
    const QImage k0 = render(keyed, fmt, 0), k25 = render(keyed, fmt, 25), k40 = render(keyed, fmt, 40);
    CHECK(gray(k0).r < 5 && std::abs(gray(k25).r - g0.r / 2) < 6 && diff(render(keyed, fmt, 60), ref) < 0.5);
    {
        Timeline tl = keyed;
        Clip orig = tl.video[0].clips[0], l = orig, r = orig;
        l.out = 24;
        r.in = 25;
        r.start = 25;
        r.id = 2;
        Keys::split(orig, l, r);
        tl.video[0].clips = {l, r};
        CHECK(diff(render(tl, fmt, 25), k25) < 0.5);
        CHECK(diff(render(tl, fmt, 40), k40) < 0.5);
    }

    // Deutsches Zahlenformat zur Laufzeit ändert nichts (LUT-Zahlen, Filter)
    const Timeline mixed = one(grade({{"liftY", 0.05}, {"gammaR", 0.1}, {"temperature", 800.0}, {"lut", inv}}));
    const QImage mixedC = render(mixed, fmt, 10);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE.utf8")) {
        const QString inv2 = QDir(dir).filePath("invert2.cube"); // neu einlesen (nicht aus dem Zwischenspeicher)
        QFile::copy(inv, inv2);
        const Timeline mixed2 = one(grade({{"liftY", 0.05}, {"gammaR", 0.1}, {"temperature", 800.0}, {"lut", inv2}}));
        CHECK(diff(render(mixed2, fmt, 10), mixedC) < 0.5);
        std::setlocale(LC_NUMERIC, "C");
    }
}

void testEditorAndFile(const QString& dir)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int v = p.timeline().video[0].clips[0].id;
    const int a = p.timeline().audio[0].clips[0].id;
    QUndoStack* undo = p.undoStack();
    auto clip = [&] { return *TimelineOps::findClip(p.timeline(), v); };

    // Ziel ohne Auswahl: oberster Videoclip am Playhead
    CHECK_EQ(ed.effectTargets(10), QVector<int>{v});
    const int s0 = undo->index();
    ed.setGradeValues({v, a}, {{AnimParam::GradeGainY, 1.3}}, 10, "Gain", "color:gain");
    ed.setGradeValues({v}, {{AnimParam::GradeGainY, 1.4}}, 10, "Gain", "color:gain"); // Ziehen = ein Schritt
    p.closeMerge();
    CHECK_EQ(undo->index(), s0 + 1);
    CHECK(EffectRegistry::has(clip(), "grade"));
    CHECK(!EffectRegistry::has(*TimelineOps::findClip(p.timeline(), a), "grade")); // nur Video
    CHECK_EQ(Keys::staticValue(clip(), AnimParam::GradeGainY), 1.4);
    undo->undo();
    CHECK(!EffectRegistry::has(clip(), "grade"));
    undo->redo();
    CHECK_EQ(Keys::staticValue(clip(), AnimParam::GradeGainY), 1.4);

    // Keyframe für die ganze Korrektur, dann Wert an anderer Stelle -> zweiter Keyframe
    ed.setGradeKeyframe({v}, 0, true);
    CHECK(Keys::animated(clip(), AnimParam::GradeLiftY) && Keys::animated(clip(), AnimParam::GradeGainY));
    ed.setGradeValues({v}, {{AnimParam::GradeGainY, 0.8}}, 100, "Gain");
    CHECK_EQ(Keys::keyTimes(clip(), {AnimParam::GradeGainY}).size(), 2);
    CHECK(std::abs(Keys::valueAt(clip(), AnimParam::GradeGainY, 50) - 1.1) < 1e-9);
    undo->undo();
    undo->undo();
    CHECK(!Keys::animated(clip(), AnimParam::GradeGainY));

    // LUT, Ausschalten, Rad zurücksetzen
    const QString lutDir = QDir(dir).filePath("projekt/luts");
    QDir().mkpath(lutDir);
    const QString lut = QDir(lutDir).filePath("look.cube");
    CHECK(writeFile(lut, cube(2, [](double&, double&, double&) {})));
    ed.setGradeLut({v}, lut);
    CHECK_EQ(EffectRegistry::value(clip(), "grade", "lut").toString(), lut);
    ed.setGradeValues({v}, {{AnimParam::GradeLiftR, 0.1}, {AnimParam::GradeLiftB, -0.05}}, 10, "Lift");
    ed.resetGrade({v}, {AnimParam::GradeLiftR, AnimParam::GradeLiftB}, 10, "Lift zurücksetzen");
    CHECK_EQ(Keys::staticValue(clip(), AnimParam::GradeLiftR), 0.0);
    CHECK_EQ(Keys::staticValue(clip(), AnimParam::GradeGainY), 1.4); // Rest bleibt
    ed.setGradeEnabled({v}, false);
    CHECK(!EffectRegistry::instance(clip(), "grade")->enabled);
    ed.setGradeEnabled({v}, true);

    // Teilen: beide Hälften behalten die Korrektur
    ed.bladeAt(v, 100);
    CHECK_EQ(p.timeline().video[0].clips.size(), 2);
    CHECK(EffectRegistry::has(p.timeline().video[0].clips[1], "grade"));

    // Speichern -> Laden -> Speichern identisch
    const QString projPath = QDir(dir).filePath("projekt/film.schneidi");
    QString err;
    CHECK(ProjectFile::save(p.data(), projPath, &err));
    ProjectData loaded;
    CHECK(ProjectFile::load(projPath, &loaded, &err));
    CHECK_EQ(ProjectFile::toJson(loaded, projPath), ProjectFile::toJson(p.data(), projPath));
    const Clip lc = loaded.timeline.video[0].clips[0];
    CHECK_EQ(EffectRegistry::value(lc, "grade", "lut").toString(), lut);
    CHECK_EQ(Keys::staticValue(lc, AnimParam::GradeGainY), 1.4);

    // Ordner verschoben: LUT relativ zur Projektdatei wiedergefunden
    const QString moved = QDir(dir).filePath("verschoben");
    CHECK(QDir().rename(QDir(dir).filePath("projekt"), moved));
    ProjectData m;
    CHECK(ProjectFile::load(QDir(moved).filePath("film.schneidi"), &m, &err));
    CHECK_EQ(EffectRegistry::value(m.timeline.video[0].clips[0], "grade", "lut").toString(),
             QDir(moved).filePath("luts/look.cube"));

    // Ganze Korrektur zurücksetzen = Effekt samt Keyframes weg, ein Undo-Schritt
    const int s1 = undo->index();
    ed.resetGrade({v}, {}, 10, "Farbkorrektur zurücksetzen");
    CHECK_EQ(undo->index(), s1 + 1);
    CHECK(!EffectRegistry::has(clip(), "grade"));

    // Alte Datei ohne Farbkorrektur und ohne relPaths lädt
    const QByteArray old = R"({"app":"schneidi","version":1,"fps":25,"media":[{"path":"/x/a.mp4","name":"a.mp4","length":100,
        "hasVideo":true}],"timeline":{"video":[{"clips":[{"id":1,"start":0,"in":0,"out":49,"media":0,
        "effects":[{"id":"grade","params":{"gainY":0.7,"lut":"/nirgends/x.cube"}}]}]}],"audio":[]}})";
    ProjectData o;
    CHECK(ProjectFile::fromJson(old, QDir(dir).filePath("alt.schneidi"), &o, &err));
    if (CHECK(o.timeline.video.size() == 1 && o.timeline.video[0].clips.size() == 1)) {
        const Clip& c = o.timeline.video[0].clips[0];
        CHECK_EQ(Keys::staticValue(c, AnimParam::GradeGainY), 0.7);
        CHECK_EQ(Keys::staticValue(c, AnimParam::GradeLiftY), 0.0); // fehlend = Standard
        CHECK_EQ(EffectRegistry::value(c, "grade", "lut").toString(), QString("/nirgends/x.cube"));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("color");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testPixels(tmp.path());
    testEditorAndFile(tmp.path());

    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Render-Test übersprungen\n");
        return Check::result();
    }
    g_media = Check::makeMedia(tmp.filePath("bars.mp4"),
                               {"-f", "lavfi", "-i", "smptebars=size=1920x1080:rate=25:duration=4", "-vf",
                                "scale=out_color_matrix=bt709", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    Mlt::Factory::init();
    std::setlocale(LC_NUMERIC, "C");
    testRender(tmp.path());
    return Check::result();
}
