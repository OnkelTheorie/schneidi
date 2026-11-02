// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/TextRegions.h"

#include "core/I18n.h"
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
#include <vector>

namespace TextRegions {

namespace {

constexpr int kVersion = 1;        // bump when the detection changes (cached results become stale)
constexpr int kStep = 30;          // brightness step of a letter's edge (0..255)
constexpr int kMinContrast = 70;   // darkest to brightest inside a text line
constexpr double kMaxHeight = 0.06; // of the picture height: larger is not HUD text
constexpr int kMinHeight = 5;      // pixels of the analysed picture
constexpr double kSamplesPerSecond = 30;

struct Segment {
    int x0, y0, x1, y1; // inclusive
};

QString cacheName(const Clip& c, int step)
{
    const QFileInfo fi(c.mediaPath);
    const QString id = QString("%1|%2|%3|%4|%5|%6|%7")
                           .arg(kVersion)
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(c.in)
                           .arg(c.out)
                           .arg(step);
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
            if (density < 0.05 || density > 0.6 || hi - lo < kMinContrast) continue;
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

QString cacheDir() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/textregions"; }

Result analyze(const ProjectFormat& format, const Clip& c, const std::atomic<bool>* cancel, std::atomic<int>* progress)
{
    Result res;
    if (c.isNested() || c.isTitle() || c.mediaPath.isEmpty()) {
        res.error = T("Namen verwischen geht nur bei Video-Clips (nicht bei Titeln, Compound- und Multicam-Clips)");
        return res;
    }
    const int step = sampleStep(format.rate);
    // Retimed clips are not cached: the key would have to hold the whole speed curve
    const bool cacheable = !c.isRetimed();
    const QString file = QDir(cacheDir()).filePath(cacheName(c, step));
    if (cacheable) {
        QFile f(file);
        if (f.open(QIODevice::ReadOnly) && read(f.readAll(), &res.samples)) {
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
    auto source = Snapshot::Source::timeline(format, tl);
    const int length = c.length();
    for (int f = 0; f < length; f += step) {
        if (cancel && cancel->load()) {
            res.canceled = true;
            res.samples.clear();
            return res;
        }
        const QImage img = source->frame(f, kSide);
        if (img.isNull()) {
            if (f == 0) {
                res.error = T("Kein Bild vom Clip");
                return res;
            }
            continue;
        }
        res.samples << NameBlur::Sample{f, detect(img)};
        if (progress) progress->store(int(qint64(f + 1) * 1000 / length));
    }
    if (cacheable) {
        QDir().mkpath(cacheDir());
        QSaveFile out(file);
        if (out.open(QIODevice::WriteOnly) && out.write(write(res.samples)) >= 0) out.commit(); // a cache: may fail
    }
    if (progress) progress->store(1000);
    return res;
}

} // namespace TextRegions
