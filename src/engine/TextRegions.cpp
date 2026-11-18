// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/TextRegions.h"

#include "core/AppPaths.h"
#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "engine/BarDetect.h"
#include "engine/Snapshot.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

namespace TextRegions {

namespace {

constexpr int kVersion = 2;        // bump when the detection changes (cached results become stale)
constexpr int kStep = 30;          // brightness step of a letter's edge (0..255)
constexpr int kMinContrast = 70;   // darkest to brightest inside a text line
constexpr double kMaxHeight = 0.06; // of the picture height: larger is not HUD text
constexpr int kMinHeight = 5;      // pixels of the analysed picture
constexpr double kSamplesPerSecond = 30;

struct Segment {
    int x0, y0, x1, y1; // inclusive
};

// The clip's frames count in the project's rate: the same in/out at 25 and 30 fps are other moments of the file
QString cacheName(const Clip& c, int step, FrameRate rate)
{
    const QFileInfo fi(c.mediaPath);
    const QString id = QString("%1|%2|%3|%4|%5|%6|%7|%8|%9/%10")
                           .arg(kVersion)
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(c.in)
                           .arg(c.out)
                           .arg(step)
                           .arg(c.pictureAspect)
                           .arg(rate.num)
                           .arg(rate.den);
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex()) + ".json";
}

QByteArray write(const QVector<NameBlur::Sample>& samples)
{
    QJsonArray all;
    for (const NameBlur::Sample& s : samples) {
        QJsonArray boxes;
        for (const QRectF& b : s.boxes) boxes.append(QJsonArray{b.x(), b.y(), b.width(), b.height()});
        all.append(QJsonObject{{"f", s.frame}, {"b", boxes}});
    }
    return QJsonDocument(QJsonObject{{"version", kVersion}, {"samples", all}}).toJson(QJsonDocument::Compact);
}

bool read(const QByteArray& data, QVector<NameBlur::Sample>* out)
{
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (o.value("version").toInt() != kVersion) return false;
    for (const QJsonValue& v : o.value("samples").toArray()) {
        NameBlur::Sample s;
        s.frame = v.toObject().value("f").toInt();
        for (const QJsonValue& b : v.toObject().value("b").toArray()) {
            const QJsonArray a = b.toArray();
            if (a.size() == 4) s.boxes << QRectF(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble());
        }
        out->append(s);
    }
    return true;
}

} // namespace

namespace {

// The clip's picture in the frame (fitted, centred; 0..1 of the frame) and the game picture inside it (0..1 of the
// picture, less with black bars), the picture's shape as shown
struct Placement {
    QRectF picture{0, 0, 1, 1};
    QRectF content{0, 0, 1, 1};
    double aspect = 0;
};

Placement placementOf(const ProjectFormat& format, const Clip& c)
{
    Placement p;
    p.aspect = MediaAspect::ofClip(c);
    if (p.aspect <= 0 || format.width <= 0 || format.height <= 0) return p;
    p.content = BarDetect::content(c.mediaPath).value_or(QRectF(0, 0, 1, 1));
    const QSizeF frame(format.width, format.height);
    const QSizeF fit = MediaAspect::fit(p.aspect, frame);
    const double fw = fit.width() / frame.width(), fh = fit.height() / frame.height();
    p.picture = QRectF((1 - fw) / 2, (1 - fh) / 2, fw, fh);
    return p;
}

// r (0..1 of `inner`) as 0..1 of what `inner` lies in
QRectF outOf(const QRectF& inner, const QRectF& r)
{
    return QRectF(inner.x() + r.x() * inner.width(), inner.y() + r.y() * inner.height(), r.width() * inner.width(),
                  r.height() * inner.height());
}

} // namespace

NameBlur::Screen screenOf(const ProjectFormat& format, const Clip& c)
{
    NameBlur::Screen s;
    const Placement p = placementOf(format, c);
    if (p.aspect <= 0) return s;
    if (p.content.height() > 0) s.aspect = p.aspect * p.content.width() / p.content.height();
    s.content = outOf(p.picture, p.content);
    return s;
}

QVector<QRectF> detect(const QImage& picture)
{
    if (picture.isNull()) return {};
    QImage img = picture;
    if (std::max(img.width(), img.height()) > kSide)
        img = img.scaled(kSide, kSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_Grayscale8);
    const int W = img.width(), H = img.height();
    if (W < 16 || H < 16) return {};
    const auto lum = [&](int x, int y) { return int(img.constScanLine(y)[x]); };

    // Letter edges along the rows: +1 rising, -1 falling
    std::vector<int8_t> edge(size_t(W) * H, 0);
    for (int y = 0; y < H; ++y) {
        const uchar* row = img.constScanLine(y);
        int8_t* e = &edge[size_t(y) * W];
        for (int x = 1; x + 1 < W; ++x) {
            const int d = int(row[x + 1]) - int(row[x - 1]);
            e[x] = d > kStep ? 1 : (d < -kStep ? -1 : 0);
        }
    }
    // Text-like where rising and falling edges are both near (strokes); smeared along the row over the letter gaps
    const int r = std::max(4, H / 120);
    std::vector<uint8_t> mark(size_t(W) * H, 0);
    std::vector<int> pos(W + 1), neg(W + 1);
    for (int y = 0; y < H; ++y) {
        const int8_t* e = &edge[size_t(y) * W];
        for (int x = 0; x < W; ++x) {
            pos[x + 1] = pos[x] + (e[x] > 0);
            neg[x + 1] = neg[x] + (e[x] < 0);
        }
        uint8_t* m = &mark[size_t(y) * W];
        for (int x = 0; x < W; ++x) {
            const int a = std::max(0, x - r), b = std::min(W, x + r + 1);
            m[x] = pos[b] - pos[a] >= 2 && neg[b] - neg[a] >= 2;
        }
    }

    // Connected areas (4-neighbours), labelled
    std::vector<int> label(size_t(W) * H, 0);
    std::vector<Segment> comps;
    std::vector<int> stack;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            if (!mark[i] || label[i]) continue;
            const int id = int(comps.size()) + 1;
            Segment s{x, y, x, y};
            stack.assign(1, int(i));
            label[i] = id;
            while (!stack.empty()) {
                const int p = stack.back();
                stack.pop_back();
                const int px = p % W, py = p / W;
                s.x0 = std::min(s.x0, px);
                s.x1 = std::max(s.x1, px);
                s.y0 = std::min(s.y0, py);
                s.y1 = std::max(s.y1, py);
                const auto push = [&](int q) {
                    if (mark[q] && !label[q]) {
                        label[q] = id;
                        stack.push_back(q);
                    }
                };
                if (px > 0) push(p - 1);
                if (px + 1 < W) push(p + 1);
                if (py > 0) push(p - W);
                if (py + 1 < H) push(p + W);
            }
            comps.push_back(s);
        }

    QVector<QRectF> out;
    const int maxH = std::max(kMinHeight, int(H * kMaxHeight));
    for (size_t k = 0; k < comps.size(); ++k) {
        const Segment& c = comps[k];
        const int id = int(k) + 1;
        if (c.x1 - c.x0 + 1 < 2 * kMinHeight || c.y1 - c.y0 + 1 < kMinHeight) continue;
        if (double(c.x1 - c.x0 + 1) * (c.y1 - c.y0 + 1) > double(W) * H / 4) continue; // foliage, gravel: not text
        // Cut into lines where the rows run (nearly) empty: two chat lines or kill feed entries stuck together
        std::vector<int> rowCount(c.y1 - c.y0 + 1, 0);
        for (int y = c.y0; y <= c.y1; ++y)
            for (int x = c.x0; x <= c.x1; ++x) rowCount[y - c.y0] += label[size_t(y) * W + x] == id;
        const int peak = *std::max_element(rowCount.begin(), rowCount.end());
        const int cut = std::max(2, peak / 4);
        for (int y = c.y0; y <= c.y1;) {
            if (rowCount[y - c.y0] < cut) {
                ++y;
                continue;
            }
            int y1 = y;
            while (y1 + 1 <= c.y1 && rowCount[y1 + 1 - c.y0] >= cut) ++y1;
            Segment s{c.x1, y, c.x0, y1};
            int marked = 0;
            for (int yy = y; yy <= y1; ++yy)
                for (int x = c.x0; x <= c.x1; ++x)
                    if (label[size_t(yy) * W + x] == id) {
                        s.x0 = std::min(s.x0, x);
                        s.x1 = std::max(s.x1, x);
                        ++marked;
                    }
            y = y1 + 1;
            const int w = s.x1 - s.x0 + 1, h = s.y1 - s.y0 + 1;
            if (h < kMinHeight || h > maxH || w < 2 * h || w > W * 0.6) continue;
            const double area = double(w) * h;
            if (marked / area < 0.5) continue; // a line of text is filled; a wire or the crosshair is not
            int edges = 0, lo = 255, hi = 0;
            for (int yy = s.y0; yy <= s.y1; ++yy)
                for (int x = s.x0; x <= s.x1; ++x) {
                    edges += edge[size_t(yy) * W + x] != 0;
                    const int v = lum(x, yy);
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            const double density = edges / area;
            if (density < 0.05 || density > 0.85 || hi - lo < kMinContrast) continue;
            out << QRectF(double(s.x0) / W, double(s.y0) / H, double(w) / W, double(h) / H);
        }
    }
    return out;
}

int sampleStep(const FrameRate& rate)
{
    const double fps = rate.fps() > 0 ? rate.fps() : 25.0;
    return std::max(1, int(std::lround(fps / kSamplesPerSecond)));
}

QString cacheDir() { return AppPaths::writable(QStandardPaths::CacheLocation) + "/textregions"; }

Result analyze(const ProjectFormat& format, const Clip& c, const std::atomic<bool>* cancel, std::atomic<int>* progress)
{
    Result res;
    if (c.isNested() || c.isTitle() || c.mediaPath.isEmpty()) {
        res.error = T("Namen verwischen geht nur bei Video-Clips (nicht bei Titeln, Verbund- und Multicam-Clips)");
        return res;
    }
    const int step = sampleStep(format.rate);
    // Found in the clip's picture (0..1 of it, cached like that), returned in the frame (0..1 of it, bars included,
    // like the windows)
    const Placement place = placementOf(format, c);
    const auto toFrame = [&place](Result& r) {
        for (NameBlur::Sample& x : r.samples)
            for (QRectF& b : x.boxes) b = outOf(place.picture, b);
    };
    // Retimed clips are not cached: the key would have to hold the whole speed curve
    const bool cacheable = !c.isRetimed();
    const QString file = QDir(cacheDir()).filePath(cacheName(c, step, format.rate));
    if (cacheable) {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly) && read(f.readAll(), &res.samples)) {
            toFrame(res);
            if (progress) progress->store(1000);
            return res;
        }
        res.samples.clear();
    }

    // The clip alone, without what the Color page and Inspector do to it, starting at frame 0 (like the tracker)
    Clip plain = c;
    plain.start = 0;
    plain.effects.clear();
    plain.keys.clear();
    plain.transform = ClipTransform{};
    plain.transIn = plain.transOut = plain.fadeIn = plain.fadeOut = 0;
    plain.renderCache = false;
    plain.enabled = true;
    plain.linkId = 0;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << plain;
    // A frame of the picture's own shape (no bars from fitting it into the timeline), so large that the game picture
    // between black bars gets kSide on its longer side: the same text size whatever the timeline's shape (a 16:9 clip
    // in a 9:16 short) and bars
    ProjectFormat own = format;
    const QRectF content = place.content;
    if (place.aspect > 0) {
        const double h = kSide / std::max(content.width() * place.aspect, content.height());
        own.height = std::clamp(int(std::lround(h / 2)) * 2, kMinFrameSize, kMaxFrameSize);
        own.width = std::clamp(int(std::lround(h * place.aspect / 2)) * 2, kMinFrameSize, kMaxFrameSize);
    }
    auto source = Snapshot::Source::timeline(own, tl);
    const int length = c.length();
    // Decoding is sequential, the detection runs on a batch of pictures at once (it costs more than decoding)
    const int batch = int(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
    for (int f = 0; f < length;) {
        if (cancel && cancel->load()) {
            res.canceled = true;
            res.samples.clear();
            return res;
        }
        QVector<int> frames;
        std::vector<std::future<QVector<QRectF>>> found;
        for (; f < length && int(frames.size()) < batch; f += step) {
            QImage img = source->frame(f);
            if (img.isNull()) {
                if (f == 0) {
                    res.error = T("Kein Bild vom Clip");
                    return res;
                }
                continue;
            }
            frames << f;
            const QRect crop(int(std::lround(content.x() * img.width())), int(std::lround(content.y() * img.height())),
                             int(std::lround(content.width() * img.width())), int(std::lround(content.height() * img.height())));
            if (crop != img.rect()) img = img.copy(crop);
            found.push_back(std::async(std::launch::async, [img = std::move(img)] { return detect(img); }));
        }
        for (int k = 0; k < frames.size(); ++k) {
            QVector<QRectF> boxes = found[size_t(k)].get();
            for (QRectF& b : boxes) b = outOf(content, b); // 0..1 of the picture
            res.samples << NameBlur::Sample{frames[k], boxes};
        }
        if (progress) progress->store(int(qint64(std::min(f, length)) * 1000 / length));
    }
    if (cacheable) {
        QDir().mkpath(cacheDir());
        QSaveFile out(file);
        if (out.open(QIODevice::WriteOnly) && out.write(write(res.samples)) >= 0) out.commit(); // a cache: may fail
    }
    toFrame(res);
    if (progress) progress->store(1000);
    return res;
}

} // namespace TextRegions
