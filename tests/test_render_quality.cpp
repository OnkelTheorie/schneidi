// Picture quality of the own MLT services (engine/video: Text, Still, Compose), measured on rendered pixels:
// - titles/subtitles with outline: the outline layer and the text layer sit exactly on top of each other (the outline
//   reaches the outline width beyond the letters on every side, also at the line ends and with a box)
// - stills: a photo much larger than the project is decoded smaller, all still producers share one memory budget
//   (a slideshow of many large photos kept each one decoded), EXIF orientation still applied
// - compose: strong downscales are prefiltered (1-px stripes shrunk 4.3:1 come out as even gray, not as random
//   stripes)

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

// A JPEG with an EXIF orientation tag (6 = turn 90 degrees clockwise to show it): APP1 segment right after SOI
QByteArray withOrientation(const QByteArray& jpg, int orientation)
{
    const QByteArray tiff = QByteArray("MM\x00\x2a\x00\x00\x00\x08", 8) + QByteArray("\x00\x01", 2) // one entry
                            + QByteArray("\x01\x12\x00\x03\x00\x00\x00\x01", 8) + char(0) + char(orientation)
                            + QByteArray(2, 0) + QByteArray(4, 0); // value (padded), no next IFD
    const QByteArray body = QByteArray("Exif\x00\x00", 6) + tiff;
    const int len = int(body.size()) + 2;
    QByteArray seg = QByteArray("\xff\xe1", 2) + char(len >> 8) + char(len & 255) + body;
    return jpg.left(2) + seg + jpg.mid(2);
}

QImage stillFrame(Mlt::Profile& profile, const QString& path)
{
    Mlt::Producer p(profile, (QString(Still::kService) + ":" + path).toUtf8().constData());
    if (!p.is_valid()) return {};
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    mlt_image_format fmt = mlt_image_rgba;
    int w = p.get_int("meta.media.width"), h = p.get_int("meta.media.height");
    const uint8_t* img = f->get_image(fmt, w, h);
    if (!img || fmt != mlt_image_rgba) return {};
    return QImage(img, w, h, w * 4, QImage::Format_RGBA8888).copy();
}

// Many large photos in a small project: decoded at most 4x the project's pixels, all of them together within the
// budget, a dropped one is read again; a turned phone photo (EXIF) comes out upright and scaled
void testStills(Mlt::Profile& profile, const QTemporaryDir& tmp)
{
    QImage photo(4000, 3000, QImage::Format_RGB32); // top half red, bottom half blue
    photo.fill(QColor(220, 30, 30));
    QPainter(&photo).fillRect(0, 1500, 4000, 1500, QColor(30, 30, 220));
    QByteArray jpg;
    {
        QBuffer buf(&jpg);
        buf.open(QIODevice::WriteOnly);
        CHECK(photo.save(&buf, "JPG", 85));
    }
    const qint64 budget = qint64(64) << 20;
    Still::setCacheBudget(budget);
    const double most = 4.0 * profile.width() * profile.height();
    std::vector<std::unique_ptr<Mlt::Producer>> keep; // all producers stay open (like the clips of a timeline)
    QString first;
    for (int i = 0; i < 12; ++i) {
        const QString path = tmp.filePath(QString("photo%1.jpg").arg(i));
        QFile f(path);
        if (!CHECK(f.open(QIODevice::WriteOnly) && f.write(jpg) == jpg.size())) return;
        f.close();
        if (i == 0) first = path;
        keep.push_back(std::make_unique<Mlt::Producer>(profile, (QString(Still::kService) + ":" + path).toUtf8().constData()));
        Mlt::Producer& p = *keep.back();
        if (!CHECK(p.is_valid())) return;
        std::unique_ptr<Mlt::Frame> fr(p.get_frame());
        mlt_image_format fmt = mlt_image_rgba;
        int w = p.get_int("meta.media.width"), h = p.get_int("meta.media.height");
        const uint8_t* img = fr->get_image(fmt, w, h);
        if (!CHECK(img && fmt == mlt_image_rgba)) return;
        if (i == 0) {
            std::printf("  still 4000x3000 in a %dx%d project: decoded %dx%d\n", profile.width(), profile.height(), w, h);
            CHECK(double(w) * h <= most * 1.01 && std::abs(double(w) / h - 4.0 / 3.0) < 0.01);
            CHECK(img[(size_t(h / 4) * w + w / 2) * 4] > 180 && img[(size_t(3 * h / 4) * w + w / 2) * 4 + 2] > 180);
        }
    }
    std::printf("  12 large stills decoded: %.1f MB held (budget %.0f MB)\n", Still::cachedBytes() / 1048576.0,
                budget / 1048576.0);
    CHECK(Still::cachedBytes() <= budget);
    // The first one was dropped: read again, same picture
    const QImage again = stillFrame(profile, first);
    CHECK(!again.isNull() && again.pixelColor(again.width() / 2, again.height() / 4).red() > 180);

    // EXIF orientation 6: shown turned clockwise (the top of the file on the right), portrait, scaled
    const QString turned = tmp.filePath("turned.jpg");
    QFile f(turned);
    if (!CHECK(f.open(QIODevice::WriteOnly))) return;
    f.write(withOrientation(jpg, 6));
    f.close();
    const QImage t = stillFrame(profile, turned);
    if (!CHECK(!t.isNull())) return;
    std::printf("  turned still: %dx%d\n", t.width(), t.height());
    CHECK(t.height() > t.width() && double(t.width()) * t.height() <= most * 1.01);
    CHECK(t.pixelColor(t.width() * 3 / 4, t.height() / 2).red() > 180 && t.pixelColor(t.width() / 4, t.height() / 2).blue() > 180);
    Still::setCacheBudget(0);
}

double stddev(const std::vector<uint8_t>& px, int w, int h, int x0, int y0, int x1, int y1)
{
    double sum = 0, sq = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double v = px[(size_t(y) * w + x) * 4];
            sum += v;
            sq += v * v;
            ++n;
        }
    const double mean = sum / n;
    return std::sqrt(std::max(0.0, sq / n - mean * mean));
}

// 1-px stripes shrunk 4.3:1 (and a zone plate 4:1 for looking at it): even gray instead of random stripes
void testDownscale(const QString& dump)
{
    const int sw = 1376, sh = 774, dw = 320, dh = 180;
    std::vector<uint8_t> src(size_t(sw) * sh * 4);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) {
            uint8_t* p = src.data() + (size_t(y) * sw + x) * 4;
            p[0] = p[1] = p[2] = (x & 1) ? 255 : 0;
            p[3] = 255;
        }
    std::vector<uint8_t> dst(size_t(dw) * dh * 4, 0);
    Compose::Affine m;
    m.a = double(dw) / sw;
    m.e = double(dh) / sh;
    Compose::draw(dst.data(), dw, dh, src.data(), sw, sh, m, 1.0);
    const double sd = stddev(dst, dw, dh, 2, 2, dw - 2, dh - 2);
    std::printf("  1-px stripes shrunk %.1f:1: spread of the gray %.1f (even gray = 0)\n", double(sw) / dw, sd);
    CHECK(sd < 8);
    // Rotated a little as well: still even
    std::fill(dst.begin(), dst.end(), 0);
    const double rad = 0.2, k = 0.25;
    Compose::Affine r{k * std::cos(rad), -k * std::sin(rad), 60, k * std::sin(rad), k * std::cos(rad), -20};
    Compose::draw(dst.data(), dw, dh, src.data(), sw, sh, r, 1.0);
    const double sdr = stddev(dst, dw, dh, 100, 70, 200, 120);
    std::printf("  1-px stripes shrunk 4:1, rotated: spread %.1f\n", sdr);
    CHECK(sdr < 12);

    if (dump.isEmpty()) return;
    // Zone plate (rings getting finer outwards) 1280x1280 -> 320x320: look at the picture
    const int zs = 1280, zd = 320;
    std::vector<uint8_t> zone(size_t(zs) * zs * 4), out(size_t(zd) * zd * 4, 0);
    for (int y = 0; y < zs; ++y)
        for (int x = 0; x < zs; ++x) {
            const double dx = x - zs / 2.0, dy = y - zs / 2.0;
            const uint8_t v = uint8_t(127.5 + 127.5 * std::cos(std::numbers::pi * (dx * dx + dy * dy) / zs));
            uint8_t* p = zone.data() + (size_t(y) * zs + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    Compose::Affine z;
    z.a = z.e = double(zd) / zs;
    Compose::draw(out.data(), zd, zd, zone.data(), zs, zs, z, 1.0);
    QImage(out.data(), zd, zd, QImage::Format_RGBA8888).save(dump + "/zoneplate-4to1.png");
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
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testStills(*profile, tmp);
    testDownscale(qEnvironmentVariable("SCHNEIDI_QUALITY_DUMP"));
    return Check::result();
}
