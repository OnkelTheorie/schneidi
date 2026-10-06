// Scopes of the Color page: computation on known images (histogram/waveform/parade/vectorscope values),
// downsampling, pixel formats, trace images, and the panel (worker thread, newest frame wins, type switch, offscreen).
// SCOPES_DUMP=dir saves the panel grabs as PNG for a visual check.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "engine/Engine.h"
#include "ui/Scopes.h"
#include "ui/ScopesPanel.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <clocale>

namespace {

using Scopes::Type;

QImage solid(int w, int h, QColor c, QImage::Format f = QImage::Format_RGBA8888)
{
    QImage img(w, h, f);
    img.fill(c);
    return img;
}

void testSolid()
{
    // Mid grey 1920x1080 -> 480x270 samples, everything in bin 128, vectorscope centre
    const Scopes::Data d = Scopes::compute(solid(1920, 1080, QColor(128, 128, 128)));
    CHECK_EQ(d.columns, 480);
    CHECK_EQ(d.rows, 270);
    CHECK_EQ(d.samples, 480 * 270);
    for (int ch = 0; ch < 4; ++ch) CHECK_EQ(d.hist[ch][128], quint32(d.samples));
    CHECK_EQ(d.hist[0][127] + d.hist[0][129], 0u);
    bool wave = true;
    for (int x = 0; x < d.columns; ++x)
        wave = wave && d.wave(x, 128) == quint32(d.rows) && d.parade(0, x, 128) == quint32(d.rows)
               && d.parade(2, x, 128) == quint32(d.rows);
    CHECK(wave);
    double cb, cr;
    Scopes::chroma(0.5, 0.5, 0.5, &cb, &cr);
    CHECK(std::abs(cb) < 1e-9 && std::abs(cr) < 1e-9);
    CHECK_EQ(d.vec(Scopes::vectorCell(0, 0)), quint32(d.samples));

    // Pure red: luma 54 (0.2126 * 255), R at 255, G/B at 0, vectorscope top left (red target direction)
    CHECK_EQ(Scopes::luma(255, 0, 0), 54);
    CHECK_EQ(Scopes::luma(255, 255, 255), 255);
    CHECK_EQ(Scopes::luma(0, 0, 0), 0);
    const Scopes::Data r = Scopes::compute(solid(320, 180, QColor(255, 0, 0)));
    CHECK_EQ(r.columns, 320);
    CHECK_EQ(r.rows, 180);
    CHECK_EQ(r.hist[0][255], quint32(r.samples));
    CHECK_EQ(r.hist[1][0], quint32(r.samples));
    CHECK_EQ(r.hist[2][0], quint32(r.samples));
    CHECK_EQ(r.hist[3][54], quint32(r.samples));
    CHECK_EQ(r.wave(10, 54), quint32(r.rows));
    CHECK_EQ(r.parade(0, 10, 255), quint32(r.rows));
    CHECK_EQ(r.parade(1, 10, 0), quint32(r.rows));
    Scopes::chroma(1, 0, 0, &cb, &cr);
    const QPoint red = Scopes::vectorCell(cb, cr);
    CHECK_EQ(r.vec(red), quint32(r.samples));
    CHECK(red.x() < Scopes::kVectorSize / 2 && red.y() < 8); // left of centre, at the top edge
    // Blue lies right (Cb > 0), green bottom left
    Scopes::chroma(0, 0, 1, &cb, &cr);
    CHECK(Scopes::vectorCell(cb, cr).x() > 240);
    Scopes::chroma(0, 1, 0, &cb, &cr);
    CHECK(Scopes::vectorCell(cb, cr).x() < 128 && Scopes::vectorCell(cb, cr).y() > 200);
}

void testPatterns()
{
    // Horizontal ramp 0..255: column x has its whole column at level x (waveform and every parade channel)
    QImage ramp(256, 64, QImage::Format_RGB32);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 256; ++x) ramp.setPixel(x, y, qRgb(x, x, x));
    const Scopes::Data d = Scopes::compute(ramp);
    CHECK_EQ(d.columns, 256);
    bool ok = true;
    for (int x = 0; x < 256; ++x) ok = ok && d.wave(x, x) == 64u && d.parade(1, x, x) == 64u;
    CHECK(ok);
    for (int i = 0; i < 256; ++i) ok = ok && d.hist[3][i] == 64u;
    CHECK(ok); // flat histogram

    // Top half black, bottom half white: each column split 50/50 between 0 and 255
    QImage split = solid(200, 100, Qt::black, QImage::Format_RGB32);
    for (int y = 50; y < 100; ++y)
        for (int x = 0; x < 200; ++x) split.setPixel(x, y, qRgb(255, 255, 255));
    const Scopes::Data s = Scopes::compute(split);
    CHECK_EQ(s.wave(0, 0), 50u);
    CHECK_EQ(s.wave(199, 255), 50u);
    CHECK_EQ(s.wave(100, 128), 0u);
    CHECK_EQ(s.hist[3][0], 10000u);
    CHECK_EQ(s.hist[3][255], 10000u);

    // 75 % colour bars hit the vectorscope targets of the panel
    const QColor bars[] = {QColor(191, 0, 0), QColor(191, 0, 191), QColor(0, 0, 191),
                           QColor(0, 191, 191), QColor(0, 191, 0), QColor(191, 191, 0)};
    QImage strip(6 * 10, 10, QImage::Format_RGBA8888);
    for (int i = 0; i < 6; ++i)
        for (int y = 0; y < 10; ++y)
            for (int x = 0; x < 10; ++x) strip.setPixelColor(i * 10 + x, y, bars[i]);
    const Scopes::Data b = Scopes::compute(strip);
    for (const QColor& c : bars) {
        double cb, cr;
        Scopes::chroma(c.redF(), c.greenF(), c.blueF(), &cb, &cr);
        const QPoint at = Scopes::vectorCell(cb, cr);
        // integer luma may move the hit by one cell
        quint32 near = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) near += b.vec(at + QPoint(dx, dy));
        CHECK_EQ(near, 100u);
    }
}

void testFormatsAndDownsampling()
{
    // Same picture in different formats -> same result (grey/indexed formats are converted)
    QImage src(300, 200, QImage::Format_RGB32);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 300; ++x) src.setPixel(x, y, qRgb(x % 256, y, (x * 7 + y) % 256));
    const Scopes::Data a = Scopes::compute(src);
    const Scopes::Data b = Scopes::compute(src.convertToFormat(QImage::Format_RGBA8888));
    const Scopes::Data c = Scopes::compute(src.convertToFormat(QImage::Format_RGB888));
    CHECK(a.luma == b.luma && a.vector == b.vector && a.hist == b.hist);
    CHECK(a.luma == c.luma && a.rgb == c.rgb);
    const Scopes::Data g = Scopes::compute(solid(64, 64, QColor(77, 77, 77), QImage::Format_Grayscale8));
    CHECK_EQ(g.hist[3][77], quint32(g.samples));

    // Downsampling: point sampling every n-th pixel, sizes rounded up
    const Scopes::Data d = Scopes::compute(solid(1000, 700, Qt::white), 480, 270);
    CHECK_EQ(d.columns, 334); // step 3
    CHECK_EQ(d.rows, 234);    // step 3
    const Scopes::Data e = Scopes::compute(solid(100, 50, Qt::white), 480, 270);
    CHECK_EQ(e.columns, 100);
    CHECK_EQ(e.rows, 50);
    CHECK(Scopes::compute(QImage()).isEmpty());

    // A full HD frame stays cheap (budget is generous for slow CI machines)
    QImage big(1920, 1080, QImage::Format_RGBA8888);
    big.fill(QColor(30, 90, 200));
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 10; ++i) Scopes::compute(big);
    CHECK(t.elapsed() < 10 * 50);
}

void testTraces()
{
    const Scopes::Data d = Scopes::compute(solid(100, 40, QColor(128, 128, 128)));
    const QImage w = Scopes::trace(d, Type::Waveform);
    CHECK_EQ(w.size(), QSize(100, Scopes::kLevels));
    CHECK_EQ(qAlpha(w.pixel(5, 255 - 128)), 255); // level 128, bottom row = level 0
    CHECK_EQ(qAlpha(w.pixel(5, 255 - 129)), 0);
    CHECK_EQ(qAlpha(w.pixel(5, 255)), 0);
    CHECK_EQ(QColor(w.pixel(5, 127)).rgb(), Theme::scopeTrace.rgb());
    const QImage p = Scopes::trace(d, Type::Parade);
    CHECK_EQ(p.size(), QSize(300, Scopes::kLevels));
    CHECK_EQ(QColor(p.pixel(5, 127)).rgb(), Theme::scopeRed.rgb());
    CHECK_EQ(QColor(p.pixel(105, 127)).rgb(), Theme::scopeGreen.rgb());
    CHECK_EQ(QColor(p.pixel(205, 127)).rgb(), Theme::scopeBlue.rgb());
    const QImage v = Scopes::trace(d, Type::Vectorscope);
    CHECK_EQ(v.size(), QSize(Scopes::kVectorSize, Scopes::kVectorSize));
    const QPoint centre = Scopes::vectorCell(0, 0);
    CHECK_EQ(qAlpha(v.pixel(centre)), 255);
    CHECK_EQ(qAlpha(v.pixel(10, 10)), 0);
    CHECK(Scopes::trace(d, Type::Histogram).isNull());
    CHECK(Scopes::trace(Scopes::Data(), Type::Waveform).isNull());

    CHECK_EQ(Scopes::intensity(0, 100), 0.0);
    CHECK(Scopes::intensity(1, 100) > 0.2);
    CHECK(Scopes::intensity(1, 100) < Scopes::intensity(10, 100));
    CHECK_EQ(Scopes::intensity(1000, 100), 1.0);
}

bool waitFor(ScopesPanel& panel, int ms = 3000)
{
    QSignalSpy spy(&panel, &ScopesPanel::analysed);
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (!spy.isEmpty() && !panel.busy()) {
            // let a pending re-analysis finish too
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            if (!panel.busy()) return true;
        }
        spy.wait(50);
    }
    return false;
}

void testPanel()
{
    const QString dump = qEnvironmentVariable("SCOPES_DUMP");
    ScopesPanel panel;
    panel.setType(Type::Waveform);
    panel.resize(480, 300);

    // Hidden: frames are only remembered
    panel.setFrame(solid(320, 180, QColor(255, 0, 0)));
    QCoreApplication::processEvents();
    CHECK(!panel.busy());
    CHECK(panel.data().isEmpty());

    // Shown: the remembered frame is analysed on the worker thread
    panel.show();
    CHECK(waitFor(panel));
    CHECK_EQ(panel.data().hist[0][255], quint32(panel.data().samples));

    // Many frames in a row (playback): newest frame wins, nothing piles up
    for (int i = 0; i < 60; ++i) panel.setFrame(solid(320, 180, QColor(i, i, i)));
    CHECK(waitFor(panel));
    CHECK_EQ(panel.data().hist[3][59], quint32(panel.data().samples));

    // A test picture: ramp + colour bars, all four scopes draw something besides the background
    QImage pic(640, 360, QImage::Format_RGBA8888);
    for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
            const int v = x * 255 / 639;
            pic.setPixelColor(x, y, y < 180 ? QColor(v, v, v) : QColor::fromHsv(x * 359 / 639, 200, 200));
        }
    panel.setFrame(pic);
    CHECK(waitFor(panel));
    auto busyPixels = [](const QImage& img) {
        int n = 0;
        const QRgb bg = Theme::viewerBg.rgb();
        for (int y = 40; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) n += QColor(img.pixel(x, y)).rgb() != bg;
        return n;
    };
    const Type types[] = {Type::Waveform, Type::Parade, Type::Vectorscope, Type::Histogram};
    const char* names[] = {"waveform", "parade", "vectorscope", "histogram"};
    for (int i = 0; i < 4; ++i) {
        panel.setType(types[i]);
        if (types[i] != Type::Waveform) CHECK(waitFor(panel));
        CHECK(panel.type() == types[i]);
        const QImage g = panel.grab().toImage();
        CHECK(busyPixels(g) > 500);
        if (!dump.isEmpty()) g.save(QDir(dump).filePath(QString("scope-%1.png").arg(names[i])));
    }
    // Combo box follows and switches the type
    auto* combo = panel.findChild<QComboBox*>();
    if (CHECK(combo)) {
        CHECK_EQ(combo->count(), 4);
        combo->setCurrentIndex(0);
        CHECK(panel.type() == Type::Waveform);
    }
}

// Offscreen screenshot of the whole Color page: scopes sit right of the wheels and show the real preview frame
void testColorPage(const QString& dir)
{
    const QString clip = Check::makeMedia(QDir(dir).filePath("bars.mp4"),
                                          {"-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=2", "-c:v",
                                           "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!clip.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({clip}, true);
    for (const auto& e : InputBindings::instance().entries())
        if (e.id == "page_color" && e.action) e.action->trigger();
    auto* scopes = w.findChild<ScopesPanel*>();
    if (!CHECK(scopes)) return;
    scopes->setType(Type::Vectorscope);
    QElapsedTimer t;
    t.start();
    // wait until the scopes analysed a real frame of the clip (bars -> many colours in the vectorscope)
    auto ready = [&] {
        if (scopes->data().isEmpty() || scopes->busy()) return false;
        int cells = 0;
        for (quint32 v : scopes->data().vector) cells += v > 0;
        return cells > 20;
    };
    while (t.elapsed() < 15000 && !ready()) QSignalSpy(scopes, &ScopesPanel::analysed).wait(100);
    CHECK(ready());
    CHECK(scopes->isVisible());
    CHECK(scopes->width() > 250 && scopes->height() > 200);
    const QImage shot = w.grab().toImage();
    const QRect area(scopes->mapTo(&w, QPoint(0, 0)), scopes->size());
    CHECK(QRect(QPoint(0, 0), shot.size()).contains(area));
    int traced = 0;
    for (int y = area.top() + 30; y <= area.bottom(); ++y)
        for (int x = area.left(); x <= area.right(); ++x) traced += QColor(shot.pixel(x, y)).rgb() != Theme::viewerBg.rgb();
    CHECK(traced > 1000);
    if (const QString dump = qEnvironmentVariable("SCOPES_DUMP"); !dump.isEmpty())
        shot.save(QDir(dump).filePath("color-page.png"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("scopes");
    testSolid();
    testPatterns();
    testFormatsAndDownsampling();
    testTraces();
    testPanel();
    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Color-Seite übersprungen\n");
        return Check::result();
    }
    QTemporaryDir tmp;
    if (CHECK(tmp.isValid())) testColorPage(tmp.path());
    return Check::result();
}
