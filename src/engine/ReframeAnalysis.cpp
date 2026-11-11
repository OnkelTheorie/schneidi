// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/ReframeAnalysis.h"

#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "core/Retime.h"
#include "engine/BarDetect.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace ReframeAnalysis {

namespace {

// Bump when the measurement changes (old results are not reused)
constexpr int kVersion = 5;
constexpr const char* kHeader = "schneidi-reframe 1";
constexpr const char* kSuffix = ".reframe";
constexpr int kSide = 128;          // longer side of the analysis picture
constexpr int kCell = 4;            // cell size in analysis pixels
constexpr double kSamplesPerSecond = 8;

QString base(const QString& file, const QRectF& content)
{
    const QFileInfo fi(file);
    QString id = QString("%1|%2|%3|%4")
                     .arg(kVersion)
                     .arg(fi.absoluteFilePath())
                     .arg(fi.size())
                     .arg(fi.lastModified().toMSecsSinceEpoch());
    if (BarDetect::hasBars(content))
        id += QString("|%1 %2 %3 %4").arg(content.x(), 0, 'f', 4).arg(content.y(), 0, 'f', 4).arg(content.width(), 0, 'f', 4)
                  .arg(content.height(), 0, 'f', 4);
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

// "<base>_<from ms>_<to ms>.reframe"
bool parseName(const QString& name, const QString& b, qint64* from, qint64* to)
{
    if (!name.startsWith(b + '_') || !name.endsWith(kSuffix)) return false;
    const QStringList parts = name.mid(b.size() + 1).chopped(int(std::strlen(kSuffix))).split('_');
    if (parts.size() != 2) return false;
    bool ok1 = false, ok2 = false;
    *from = parts[0].toLongLong(&ok1);
    *to = parts[1].toLongLong(&ok2);
    return ok1 && ok2 && *to >= *from;
}

std::optional<Samples> readSamples(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QList<QByteArray> lines = f.readAll().split('\n');
    if (lines.size() < 2 || lines[0] != kHeader) return std::nullopt;
    bool ok = false;
    const int n = lines[1].toInt(&ok);
    if (!ok || n < 0 || lines.size() < n + 2) return std::nullopt;
    Samples out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QList<QByteArray> v = lines[i + 2].split(' ');
        if (v.size() != 5) return std::nullopt;
        Sample s;
        bool a = false, b = false, c = false, d = false;
        s.seconds = v[0].toDouble(&a);
        s.x = v[1].toDouble(&b);
        s.y = v[2].toDouble(&c);
        s.weight = v[3].toDouble(&d);
        s.cut = v[4] == "1";
        if (!a || !b || !c || !d) return std::nullopt;
        out << s;
    }
    return out;
}

QByteArray writeSamples(const Samples& samples)
{
    QByteArray out = QByteArray(kHeader) + "\n" + QByteArray::number(samples.size()) + "\n";
    for (const Sample& s : samples)
        out += QByteArray::number(s.seconds, 'f', 4) + ' ' + QByteArray::number(s.x, 'f', 4) + ' '
               + QByteArray::number(s.y, 'f', 4) + ' ' + QByteArray::number(s.weight, 'f', 3) + ' '
               + (s.cut ? '1' : '0') + '\n';
    return out;
}

// Features of one sampled picture per cell
struct Cells {
    std::vector<float> motion, detail, skin, luma;
    bool cut = false;
    double seconds = 0;
};

float quantile(std::vector<float> v, double q)
{
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

// Subject centre of one sample: every cell scored by how much it stands out (motion, detail, contrast to its
// surroundings, skin), the strongest area found, its weighted centre taken. How sure: the share of all scores that
// lies in that area (one subject ~1, a busy picture spread everywhere ~0.1) and how strong they are at all.
Sample locate(const Cells& c, const std::vector<float>& still, int cw, int ch)
{
    const size_t n = size_t(cw) * size_t(ch);
    const auto at = [cw](int x, int y) { return size_t(y) * size_t(cw) + size_t(x); };
    std::vector<float> detail(n), contrast(n);
    for (size_t i = 0; i < n; ++i) detail[i] = c.detail[i] * still[i];
    // Brightness against the cells around (5 x 5): a bright object on a dark ground, a face against a wall
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            double sum = 0;
            int k = 0;
            for (int v = std::max(0, y - 2); v <= std::min(ch - 1, y + 2); ++v)
                for (int u = std::max(0, x - 2); u <= std::min(cw - 1, x + 2); ++u) sum += c.luma[at(u, v)], ++k;
            contrast[at(x, y)] = float(std::abs(c.luma[at(x, y)] - sum / k)) * still[at(x, y)];
        }
    const float mMed = quantile(c.motion, 0.5), mTop = quantile(c.motion, 0.98);
    const float dMed = quantile(detail, 0.5), dTop = quantile(detail, 0.98);
    const float cMed = quantile(contrast, 0.5), cTop = quantile(contrast, 0.98);
    // Floors: picture noise and compression must not count as a subject
    const float mRange = std::max(mTop - mMed, 6.0f), dRange = std::max(dTop - dMed, 10.0f),
                cRange = std::max(cTop - cMed, 12.0f);
    double skinShare = 0;
    for (float s : c.skin) skinShare += s > 0.5f;
    skinShare /= double(n);
    // Skin everywhere is a wall, sand or a wooden table, not a face
    const double skinTrust = std::clamp((0.45 - skinShare) / 0.3, 0.0, 1.0);

    std::vector<double> score(n);
    double total = 0;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const size_t i = at(x, y);
            const double m = std::clamp((c.motion[i] - mMed) / mRange, 0.0f, 1.0f);
            const double d = std::clamp((detail[i] - dMed) / dRange, 0.0f, 1.0f);
            const double k = std::clamp((contrast[i] - cMed) / cRange, 0.0f, 1.0f);
            const double px = (x + 0.5) / cw, py = (y + 0.5) / ch;
            const double prior = std::exp(-((px - 0.5) * (px - 0.5) + (py - 0.5) * (py - 0.5)) / (2 * 0.3 * 0.3));
            score[i] = (m + 0.6 * d + 0.6 * k + 0.8 * c.skin[i] * skinTrust) * (0.5 + 0.5 * prior);
            total += score[i];
        }
    Sample out;
    out.seconds = c.seconds;
    out.cut = c.cut;
    if (total <= 1e-9) return out;
    // The strongest area: scores summed over a box of about a sixth of the picture
    const int rx = std::max(1, cw / 12), ry = std::max(1, ch / 8);
    double best = -1;
    int bx = cw / 2, by = ch / 2;
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            double sum = 0;
            for (int v = std::max(0, y - ry); v <= std::min(ch - 1, y + ry); ++v)
                for (int u = std::max(0, x - rx); u <= std::min(cw - 1, x + rx); ++u) sum += score[at(u, v)];
            if (sum > best) best = sum, bx = x, by = y;
        }
    // Its weighted centre (a little wider box, so a subject larger than the box is taken whole)
    double sw = 0, sx = 0, sy = 0, inside = 0;
    for (int v = std::max(0, by - 2 * ry); v <= std::min(ch - 1, by + 2 * ry); ++v)
        for (int u = std::max(0, bx - 2 * rx); u <= std::min(cw - 1, bx + 2 * rx); ++u) {
            const double s = score[at(u, v)];
            inside += s;
            sw += s * s;
            sx += s * s * (u + 0.5) / cw;
            sy += s * s * (v + 0.5) / ch;
        }
    if (sw <= 1e-12) return out;
    out.x = sx / sw;
    out.y = sy / sw;
    const double area = double((4 * rx + 1) * (4 * ry + 1)) / double(n); // share of the picture in the wider box
    const double share = inside / total;
    const double focus = std::clamp((share - area) / (0.5 - area), 0.0, 1.0);
    const double strength = std::clamp(total / (double(n) * 0.006), 0.0, 1.0);
    out.weight = focus * strength;
    return out;
}

} // namespace

QString cacheDir() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/reframe"; }

std::optional<Need> need(const Clip& c, FrameRate rate, int fileLength)
{
    if (!Reframe::canReframe(c)) return std::nullopt;
    const double fps = rate.fps() > 0 ? rate.fps() : 25.0;
    double a = c.in, b = c.out;
    if (c.isRetimed()) {
        const RetimeMap map(c, fileLength);
        a = map.pictureFrame(c.in);
        b = map.pictureFrame(c.out);
    }
    if (c.freeze) a = b = c.isRetimed() ? RetimeMap(c, fileLength).pictureFrame(c.stillFrame()) : c.stillFrame();
    Need n;
    n.file = QFileInfo(c.mediaPath).absoluteFilePath();
    // Whole tenths of a second (a clip trimmed a little reuses the result)
    n.from = std::max(0.0, std::floor(std::min(a, b) / fps * 10) / 10);
    n.to = std::ceil((std::max(a, b) + 1) / fps * 10) / 10;
    return n;
}

std::optional<Samples> cached(const Need& n)
{
    const QString b = base(n.file, n.content);
    const QDir dir(cacheDir());
    const qint64 from = qint64(std::llround(n.from * 1000)), to = qint64(std::llround(n.to * 1000));
    QString best;
    qint64 bestLength = -1;
    for (const QString& name : dir.entryList({b + "_*" + kSuffix}, QDir::Files)) {
        qint64 f = 0, t = 0;
        if (!parseName(name, b, &f, &t) || f > from || t < to) continue;
        if (best.isEmpty() || t - f < bestLength) best = dir.filePath(name), bestLength = t - f; // the smallest: fewer lines
    }
    if (best.isEmpty()) return std::nullopt;
    return readSamples(best);
}

namespace {

std::optional<Samples> measure(const Need& n, QString* error, const std::atomic<bool>* cancel,
                               const std::function<void(int)>& progress)
{
    auto fail = [&](const QString& msg) -> std::optional<Samples> {
        if (error) *error = msg;
        return std::nullopt;
    };
    if (auto c = cached(n)) {
        if (progress) progress(1000);
        return c;
    }
    if (!QFileInfo::exists(n.file)) return fail(T("Datei nicht gefunden: %1").arg(n.file));
    Video::Reader::Options ro;
    ro.hardware = false; // in order through the range: the software decoder keeps up
    std::unique_ptr<Video::Reader> reader = Video::Reader::open(n.file, ro);
    if (!reader) return fail(T("Datei lässt sich nicht öffnen: %1").arg(n.file));
    reader->setFrameRate(AVRational{25, 1});
    const Video::Info& info = reader->info();
    const double srcFps = info.frameRate.num > 0 && info.frameRate.den > 0 ? av_q2d(info.frameRate) : 25.0;
    const bool sideways = std::lround(std::abs(info.rotation)) % 180 == 90;
    const double sar = info.sar.num > 0 && info.sar.den > 0 ? av_q2d(info.sar) : 1.0;
    const QRectF content = BarDetect::hasBars(n.content) ? n.content : QRectF(0, 0, 1, 1);
    const double dar = (info.height > 0 ? (sideways ? double(info.height) / (info.width * sar) : info.width * sar / info.height)
                                        : 16.0 / 9.0)
                       * content.width() / std::max(0.01, content.height());
    // Analysis picture: whole cells, even sides
    int aw = kSide, ah = kSide;
    if (dar >= 1) ah = std::max(kCell * 2, int(std::lround(kSide / dar / kCell)) * kCell);
    else aw = std::max(kCell * 2, int(std::lround(kSide * dar / kCell)) * kCell);
    const int cw = aw / kCell, ch = ah / kCell;
    const size_t cells = size_t(cw) * size_t(ch);

    const int64_t first = int64_t(std::floor(n.from * srcFps));
    const int64_t last = std::max(first, int64_t(std::ceil(n.to * srcFps)));
    const int step = std::max(1, int(std::lround(srcFps / kSamplesPerSecond)));
    const int minShot = std::max(2, int(std::lround(srcFps * 0.3)));

    SwsContext* sws = nullptr;
    std::vector<uint8_t> yuv(size_t(aw) * size_t(ah) * 3 / 2);
    uint8_t* planes[4] = {yuv.data(), yuv.data() + size_t(aw) * size_t(ah),
                          yuv.data() + size_t(aw) * size_t(ah) * 5 / 4, nullptr};
    int strides[4] = {aw, aw / 2, aw / 2, 0};
    std::vector<uint8_t> prevFrame, prevSample;
    std::vector<int> hist(32), prevHist(32);
    std::vector<Cells> picked;
    int64_t lastCut = first - minShot, lastSample = first - step;
    // The file's end in positions (0 = unknown: a repeated picture ends the measuring)
    const int64_t endFrame = info.duration > 0 ? int64_t(std::ceil(double(info.duration) / AV_TIME_BASE * srcFps)) : 0;
    for (int64_t pos = first; pos <= last; ++pos) {
        if (cancel && cancel->load()) break;
        const auto pic = reader->picture(pos);
        if (!pic || !pic->frame) break;
        if (pic->position < pos) {
            // The picture before again: past the end of the file, or a gap in its timestamps (variable frame rate:
            // the reader shows the held picture there) - measured already, go on after it
            if (endFrame <= 0 || pos >= endFrame) break;
            continue;
        }
        pos = pic->position; // positions the file skips (a reader answers a request before its first picture)
        const AVFrame* f = pic->frame;
        // Only the content (bars cut off): a cropped reference of the picture
        AVFrame* cropped = nullptr;
        if (BarDetect::hasBars(content) && (cropped = av_frame_clone(f))) {
            cropped->crop_left = size_t(std::lround(content.left() * f->width));
            cropped->crop_right = size_t(std::max(0L, std::lround((1 - content.right()) * f->width)));
            cropped->crop_top = size_t(std::lround(content.top() * f->height));
            cropped->crop_bottom = size_t(std::max(0L, std::lround((1 - content.bottom()) * f->height)));
            if (av_frame_apply_cropping(cropped, AV_FRAME_CROP_UNALIGNED) >= 0) f = cropped;
        }
        const std::unique_ptr<AVFrame, void (*)(AVFrame*)> keep(cropped, [](AVFrame* x) { av_frame_free(&x); });
        sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), aw, ah, AV_PIX_FMT_YUV420P,
                                   SWS_AREA, nullptr, nullptr, nullptr);
        if (!sws) break;
        sws_scale(sws, f->data, f->linesize, 0, f->height, planes, strides);
        const uint8_t* Y = planes[0];
        const size_t pixels = size_t(aw) * size_t(ah);

        // Cut: the brightness distribution changes a lot and the pictures differ
        std::fill(hist.begin(), hist.end(), 0);
        for (size_t i = 0; i < pixels; ++i) ++hist[Y[i] >> 3];
        bool cut = false;
        if (!prevFrame.empty()) {
            int h = 0;
            for (int i = 0; i < 32; ++i) h += std::abs(hist[size_t(i)] - prevHist[size_t(i)]);
            double diff = 0;
            for (size_t i = 0; i < pixels; ++i) diff += std::abs(int(Y[i]) - int(prevFrame[i]));
            diff /= double(pixels);
            cut = h / (2.0 * double(pixels)) > 0.35 && diff > 18 && pos - lastCut >= minShot;
        }
        prevHist = hist;
        prevFrame.assign(Y, Y + pixels);
        if (cut) {
            lastCut = pos;
            prevSample.clear(); // no motion across a cut
        }
        if (!cut && pos - lastSample < step && pos != first) continue;
        lastSample = pos;

        Cells c;
        c.cut = cut;
        c.seconds = double(pic->position) / srcFps;
        c.motion.assign(cells, 0);
        c.detail.assign(cells, 0);
        c.skin.assign(cells, 0);
        c.luma.assign(cells, 0);
        for (int y = 0; y < ah; ++y)
            for (int x = 0; x < aw; ++x) {
                const size_t i = size_t(y) * size_t(aw) + size_t(x);
                const size_t cell = size_t(y / kCell) * size_t(cw) + size_t(x / kCell);
                const int gx = std::abs(int(Y[y * aw + std::min(x + 1, aw - 1)]) - int(Y[y * aw + std::max(x - 1, 0)]));
                const int gy = std::abs(int(Y[std::min(y + 1, ah - 1) * aw + x]) - int(Y[std::max(y - 1, 0) * aw + x]));
                c.detail[cell] += float(gx + gy);
                c.luma[cell] += float(Y[i]);
                if (!prevSample.empty()) c.motion[cell] += float(std::abs(int(Y[i]) - int(prevSample[i])));
            }
        // Skin: a common chroma box (Cb 77..127, Cr 133..173) on the half-size chroma planes, not too dark
        for (int y = 0; y < ah / 2; ++y)
            for (int x = 0; x < aw / 2; ++x) {
                const int cb = planes[1][y * (aw / 2) + x], cr = planes[2][y * (aw / 2) + x];
                const int luma = Y[(2 * y) * aw + 2 * x];
                if (cb >= 77 && cb <= 127 && cr >= 133 && cr <= 173 && luma > 50)
                    c.skin[size_t(2 * y / kCell) * size_t(cw) + size_t(2 * x / kCell)] += 1;
            }
        const float area = float(kCell * kCell);
        for (size_t i = 0; i < cells; ++i) {
            c.motion[i] /= area;
            c.detail[i] /= area;
            c.luma[i] /= area;
            c.skin[i] /= area / 4;
        }
        prevSample.assign(Y, Y + pixels);
        picked.push_back(std::move(c));
        if (progress) progress(int(std::min<int64_t>(999, (pos - first + 1) * 1000 / (last - first + 1))));
    }
    sws_freeContext(sws);
    reader.reset();
    if (cancel && cancel->load()) return fail(T("Abgebrochen."));
    if (picked.empty()) return fail(T("Kein Bild vom Clip"));

    // Cells that hardly ever change while the rest of the picture does keep their detail out (HUD, logo, background)
    std::vector<float> activity(cells, 0);
    for (const Cells& c : picked)
        for (size_t i = 0; i < cells; ++i) activity[i] += c.motion[i];
    double mean = 0;
    for (float a : activity) mean += a;
    mean /= double(cells);
    std::vector<float> still(cells, 1);
    if (picked.size() > 2 && mean > 0)
        for (size_t i = 0; i < cells; ++i) still[i] = float(std::clamp(activity[i] / (0.5 * mean), 0.0, 1.0));

    // A moving camera (a pan, a game's view turning) points at what matters itself: the more of the picture moves,
    // the less a measurement off the middle counts - for the whole range (a first-person game) and per sample
    const auto movingShare = [cells](const Cells& c) {
        if (c.cut) return 0.0;
        int moving = 0;
        for (float m : c.motion) moving += m > 8;
        return double(moving) / double(cells);
    };
    double rangeShare = 0;
    for (const Cells& c : picked) rangeShare += movingShare(c);
    rangeShare /= double(picked.size());
    const double rangeTrust = std::clamp(1.0 - (rangeShare - 0.2) / 0.2, 0.0, 1.0);
    Samples out;
    out.reserve(int(picked.size()));
    for (const Cells& c : picked) {
        Sample s = locate(c, still, cw, ch);
        s.weight *= rangeTrust * std::clamp(1.0 - (movingShare(c) - 0.3) / 0.3, 0.0, 1.0);
        out << s;
    }
    if (progress) progress(1000);

    QDir().mkpath(cacheDir());
    const QString target = QDir(cacheDir()).filePath(QString("%1_%2_%3%4")
                                                         .arg(base(n.file, n.content))
                                                         .arg(qint64(std::llround(n.from * 1000)))
                                                         .arg(qint64(std::llround(n.to * 1000)))
                                                         .arg(kSuffix));
    QSaveFile f(target);
    if (f.open(QIODevice::WriteOnly) && f.write(writeSamples(out)) >= 0) f.commit(); // a cache: failing to store is fine
    return out;
}

} // namespace

std::optional<Samples> analyze(const Need& n, QString* error, const std::atomic<bool>* cancel, std::atomic<int>* progress)
{
    return measure(n, error, cancel, [progress](int p) {
        if (progress) *progress = p;
    });
}

QVector<Reframe::Target> targets(const Clip& c, FrameRate rate, int fileLength, const Samples& samples)
{
    struct Item {
        Reframe::Target t;
        int index;
    };
    QVector<Item> items;
    const double fps = rate.fps() > 0 ? rate.fps() : 25.0;
    if (c.freeze) {
        // One picture: the sample closest to it
        const double at = (c.isRetimed() ? RetimeMap(c, fileLength).pictureFrame(c.stillFrame()) : c.stillFrame()) / fps;
        const Sample* best = nullptr;
        for (const Sample& s : samples)
            if (!best || std::abs(s.seconds - at) < std::abs(best->seconds - at)) best = &s;
        if (best) return {Reframe::Target{0, best->x, best->y, best->weight, false}};
        return {};
    }
    const RetimeMap map(c, fileLength);
    for (int i = 0; i < samples.size(); ++i) {
        const Sample& s = samples[i];
        const double file = s.seconds * fps;
        const int material = c.isRetimed() ? map.materialForPicture(file) : int(std::ceil(file - 1e-6));
        const int t = material - c.in;
        if (t < 0 || t >= c.length()) continue;
        items << Item{Reframe::Target{t, s.x, s.y, s.weight, false}, i};
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.t.frame < b.t.frame; });
    QVector<Reframe::Target> out;
    for (int k = 0; k < items.size(); ++k) {
        Reframe::Target t = items[k].t;
        if (k > 0) {
            // A cut between the two samples (either direction: reversed clips)
            const int a = std::min(items[k - 1].index, items[k].index), b = std::max(items[k - 1].index, items[k].index);
            for (int j = a + 1; j <= b && !t.cut; ++j) t.cut = samples[j].cut;
            if (t.frame == out.last().frame) {
                // Two samples on one clip frame (a cut is sampled right after a regular sample; a fast source or
                // a sped-up clip): the one after a cut wins, else the next target would no longer see the cut
                if (t.cut) out.last() = t;
                continue;
            }
        }
        out << t;
    }
    if (out.isEmpty() && !samples.isEmpty()) {
        // Shorter than the sampling step (or a still image): the sample nearest to the clip's first picture
        const double at = (c.isRetimed() ? map.pictureFrame(c.in) : c.in) / fps;
        const Sample* best = &samples.front();
        for (const Sample& s : samples)
            if (std::abs(s.seconds - at) < std::abs(best->seconds - at)) best = &s;
        out << Reframe::Target{0, best->x, best->y, best->weight, false};
    }
    return out;
}

QHash<int, QVector<Reframe::Key>> paths(const QVector<Clip>& clips, const ProjectFormat& format, double smoothness,
                                        const std::function<int(const QString&)>& lengthOf, QStringList* errors,
                                        const std::atomic<bool>* cancel, std::atomic<int>* progress,
                                        QHash<QString, QRectF>* contents)
{
    QHash<int, QVector<Reframe::Key>> out;
    // Progress: every clip counts by its length
    qint64 total = 0, done = 0;
    for (const Clip& c : clips) total += std::max(1, c.length());
    for (const Clip& c : clips) {
        if (cancel && cancel->load()) return {};
        const int length = lengthOf ? lengthOf(c.mediaPath) : 0;
        auto n = need(c, format.rate, length);
        if (!n) continue;
        if (contents) {
            if (!contents->contains(c.mediaPath)) {
                QString barError;
                const auto r = BarDetect::content(c.mediaPath, &barError, cancel);
                if (cancel && cancel->load()) return {};
                contents->insert(c.mediaPath, r.value_or(QRectF(0, 0, 1, 1)));
            }
            n->content = contents->value(c.mediaPath);
        }
        QString error;
        const qint64 before = done, weight = std::max(1, c.length());
        const std::optional<Samples> samples = measure(*n, &error, cancel, [&](int permille) {
            if (progress) *progress = int((before * 1000 + weight * permille) / std::max<qint64>(1, total));
        });
        done += std::max(1, c.length());
        if (!samples) {
            if (cancel && cancel->load()) return {};
            if (errors) *errors << error;
            continue;
        }
        Reframe::PathParams pp;
        pp.smoothness = smoothness;
        pp.fps = format.rate.fps();
        const Reframe::Fill f = Reframe::fill(MediaAspect::ofClip(c), QSizeF(format.size()), n->content);
        pp.windowW = f.windowW;
        pp.windowH = f.windowH;
        out.insert(c.id, Reframe::path(targets(c, format.rate, length, *samples), pp));
    }
    if (progress) *progress = 1000;
    return out;
}

} // namespace ReframeAnalysis
