// Picture quality of the own MLT services (engine/video: Text, Still, Compose), measured on rendered pixels:
// - titles/subtitles with outline: the outline layer and the text layer sit exactly on top of each other (the outline
//   reaches the outline width beyond the letters on every side, also at the line ends and with a box)

#include "check.h"

#include "core/ProjectFormat.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilderDetail.h"
#include "engine/video/Compose.h"
#include "engine/video/Still.h"

#include <Mlt.h>
#include <QApplication>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <cstring>
#include <numbers>
#include <functional>
#include <memory>
#include <vector>

namespace {

// Bounding box of the pixels that pass a test (empty: right < left)
struct Box {
    int left = 1 << 30, top = 1 << 30, right = -1, bottom = -1;
    bool empty() const { return right < left; }
};

Box bounds(const QImage& img, const std::function<bool(QRgb)>& pass)
{
    Box b;
    for (int y = 0; y < img.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (pass(row[x])) {
                b.left = std::min(b.left, x);
                b.right = std::max(b.right, x);
                b.top = std::min(b.top, y);
                b.bottom = std::max(b.bottom, y);
            }
    }
    return b;
}

// Frame 0 of a cut at project size as straight-alpha ARGB (QImage keeps the RGBA bytes in its own order)
QImage frameOf(Mlt::Producer* cut, int w, int h)
{
    if (!cut) return {};
    std::unique_ptr<Mlt::Producer> owner(cut);
    cut->seek(0);
    std::unique_ptr<Mlt::Frame> f(cut->get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    int fw = w, fh = h;
    const uint8_t* img = f->get_image(fmt, fw, fh);
    if (!img || fmt != mlt_image_rgba || fw != w || fh != h) return {};
    return QImage(img, fw, fh, fw * 4, QImage::Format_RGBA8888).convertToFormat(QImage::Format_ARGB32);
}

// White text, red outline T px, optional opaque blue box with margin pad: the outline must stick out T px beyond the
// letters on all four sides (before: the outline layer was laid out with a different scale than the text layer, at
// the line ends the outline was missing), the box must stand pad px around the outline.
void testOutline(Mlt::Profile& profile, const char* name, bool subtitle, double T, bool box, double pad)
{
    TitleStyle t;
    t.text = subtitle ? "HALLO WELT HALLO WELT HALLO" : "HALLO WELT HALLO\nZWEITE ZEILE HH";
    t.size = 72;
    t.color = Qt::white;
    t.outlineOn = true;
    t.outlineWidth = T;
    t.outlineColor = QColor(255, 0, 0);
    t.boxOn = box;
    t.boxPad = pad;
    t.boxColor = QColor(0, 0, 255);
    t.align = 1;
    t.posY = subtitle ? 60 : 0;
    Mlt::Producer* cut = nullptr;
    if (subtitle) {
        cut = TimelineBuilderDetail::subtitleCut(profile, t, t.text, 10);
    } else {
        Clip c;
        c.id = 1;
        c.kind = ClipKind::Title;
        c.out = 9;
        c.title = t;
        cut = TimelineBuilderDetail::titleCut(profile, c, 0, 10);
    }
    const QImage img = frameOf(cut, profile.width(), profile.height());
    if (!CHECK(!img.isNull())) return;
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_QUALITY_DUMP"); !dump.isEmpty()) { // over gray to look at
        QImage shown(img.size(), QImage::Format_ARGB32);
        shown.fill(QColor(90, 90, 90));
        QPainter(&shown).drawImage(0, 0, img);
        shown.save(QString("%1/%2.png").arg(dump, QString(name).replace(' ', '_').replace(',', "").replace('+', "and")));
    }
    const Box text = bounds(img, [](QRgb p) { return qAlpha(p) > 128 && qGreen(p) > 128; });
    const Box ink = bounds(img, [](QRgb p) { return qAlpha(p) > 128 && qRed(p) > 128; }); // text + outline
    if (!CHECK(!text.empty() && !ink.empty())) return;
    const int l = text.left - ink.left, r = ink.right - text.right, u = text.top - ink.top, d = ink.bottom - text.bottom;
    std::printf("  %-28s outline beyond the letters: left %d right %d top %d bottom %d (T %.0f)\n", name, l, r, u, d, T);
    const double tol = 1.5;
    CHECK(std::abs(l - T) <= tol && std::abs(r - T) <= tol && std::abs(u - T) <= tol && std::abs(d - T) <= tol);
    if (box) {
        const Box all = bounds(img, [](QRgb p) { return qAlpha(p) > 128; });
        const int bl = ink.left - all.left, br = all.right - ink.right;
        std::printf("  %-28s box beyond the outline: left %d right %d (pad %.0f)\n", name, bl, br, pad);
        // The glyphs' side bearings add a few pixels; both sides alike
        CHECK(bl >= pad - 1 && br >= pad - 1 && std::abs(bl - br) <= 2);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("render_quality");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    auto profile = makeProfile(ProjectFormat{1280, 720, {25, 1}});

    testOutline(*profile, "title, outline 8", false, 8, false, 0);
    testOutline(*profile, "title, outline 4 + box 20", false, 4, true, 20);
    testOutline(*profile, "subtitle, outline 6", true, 6, false, 0);
    testOutline(*profile, "subtitle, outline 6 + box 10", true, 6, true, 10);
    return Check::result();
}
