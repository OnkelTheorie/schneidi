// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/CompCutout.h"

#include "core/AppPaths.h"
#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "engine/Tracker.h"
#include "engine/video/Compose.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Picture::Comp {

namespace {

constexpr quint32 kMagic = 0x53435554; // "SCUT"
constexpr quint32 kFileVersion = 1;
constexpr int kAnalysisEdge = 640;     // longest frame edge the model's picture is taken from (it sees 192 x 192)
constexpr int kMinRows = 16;

inline uint8_t byteOf(float v)
{
    const float b = v * 255.0f + 0.5f;
    return uint8_t(b <= 0.0f ? 0 : b >= 255.0f ? 255 : int(b));
}

// Mean over a (2r+1)^2 square, clipped at the edges (divided by the pixels inside)
void boxMean(const std::vector<float>& in, std::vector<float>& out, int w, int h, int r)
{
    std::vector<float> tmp(in.size());
    std::vector<double> pre(size_t(std::max(w, h)) + 1);
    for (int y = 0; y < h; ++y) {
        const float* row = in.data() + size_t(y) * w;
        pre[0] = 0;
        for (int x = 0; x < w; ++x) pre[size_t(x) + 1] = pre[size_t(x)] + row[x];
        float* o = tmp.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const int lo = std::max(0, x - r), hi = std::min(w - 1, x + r);
            o[x] = float((pre[size_t(hi) + 1] - pre[size_t(lo)]) / (hi - lo + 1));
        }
    }
    out.resize(in.size());
    for (int x = 0; x < w; ++x) {
        pre[0] = 0;
        for (int y = 0; y < h; ++y) pre[size_t(y) + 1] = pre[size_t(y)] + tmp[size_t(y) * w + x];
        for (int y = 0; y < h; ++y) {
            const int lo = std::max(0, y - r), hi = std::min(h - 1, y + r);
            out[size_t(y) * w + x] = float((pre[size_t(hi) + 1] - pre[size_t(lo)]) / (hi - lo + 1));
        }
    }
}

// Max (grow) or min (shrink) over a (2r+1)^2 square, separable
std::vector<uint8_t> extreme(const std::vector<uint8_t>& m, int w, int h, int r, bool grow)
{
    if (r <= 0) return m;
    std::vector<uint8_t> tmp(m.size()), out(m.size());
    const auto pick = [grow](uint8_t a, uint8_t b) { return grow ? std::max(a, b) : std::min(a, b); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = m[size_t(y) * w + x];
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) v = pick(v, m[size_t(y) * w + k]);
            tmp[size_t(y) * w + x] = v;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t v = tmp[size_t(y) * w + x];
            for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) v = pick(v, tmp[size_t(k) * w + x]);
            out[size_t(y) * w + x] = v;
        }
    return out;
}

struct Sample {
    int i0, i1;
    float t;
};

// Bilinear source positions of each destination pixel along one axis (pixel centres)
std::vector<Sample> samples(int src, int dst)
{
    std::vector<Sample> s(size_t(std::max(0, dst)));
    const float k = float(src) / float(std::max(1, dst));
    for (int i = 0; i < dst; ++i) {
        const float f = std::clamp((i + 0.5f) * k - 0.5f, 0.0f, float(src - 1));
        const int i0 = int(f);
        s[size_t(i)] = {i0, std::min(src - 1, i0 + 1), f - float(i0)};
    }
    return s;
}

} // namespace

QSize matteSize(QSize picture, int shortSide)
{
    const int w = std::max(1, picture.width()), h = std::max(1, picture.height());
    const int shortest = std::min(w, h);
    if (shortest <= shortSide) return {w, h};
    const double k = double(shortSide) / shortest;
    return {std::max(1, int(std::lround(w * k))), std::max(1, int(std::lround(h * k)))};
}

std::vector<uint8_t> resizePlane(const uint8_t* src, int sw, int sh, int dw, int dh)
{
    std::vector<uint8_t> out(size_t(std::max(0, dw)) * size_t(std::max(0, dh)));
    if (!src || sw <= 0 || sh <= 0 || out.empty()) return out;
    const std::vector<Sample> xs = samples(sw, dw), ys = samples(sh, dh);
    for (int y = 0; y < dh; ++y) {
        const Sample sy = ys[size_t(y)];
        const uint8_t* r0 = src + size_t(sy.i0) * sw;
        const uint8_t* r1 = src + size_t(sy.i1) * sw;
        uint8_t* o = out.data() + size_t(y) * dw;
        for (int x = 0; x < dw; ++x) {
            const Sample sx = xs[size_t(x)];
            const float top = r0[sx.i0] + (r0[sx.i1] - r0[sx.i0]) * sx.t;
            const float bottom = r1[sx.i0] + (r1[sx.i1] - r1[sx.i0]) * sx.t;
            o[x] = uint8_t(top + (bottom - top) * sy.t + 0.5f);
        }
    }
    return out;
}

void smoothFrame(const std::vector<const uint8_t*>& window, int center, size_t pixels, uint8_t* out, int tolerance)
{
    const int n = int(window.size());
    if (n == 0 || center < 0 || center >= n) return;
    const int radius = std::max(center, n - 1 - center);
    const uint8_t* c = window[size_t(center)];
    tolerance = std::max(2, tolerance);
    for (size_t i = 0; i < pixels; ++i) {
        const int v = c[i];
        const int half = tolerance / 2;
        int64_t sum = int64_t(v) * (radius + 1) * half, weight = int64_t(radius + 1) * half;
        for (int k = 0; k < n; ++k) {
            if (k == center) continue;
            const int o = window[size_t(k)][i];
            const int diff = std::abs(o - v);
            if (diff >= tolerance) continue;
            const int64_t wt = int64_t(radius + 1 - std::abs(k - center)) * std::min(half, tolerance - diff);
            sum += wt * o;
            weight += wt;
        }
        out[i] = uint8_t((sum + weight / 2) / weight);
    }
}

void smoothMattes(MatteSeq& m, int radius, int tolerance)
{
    const int n = int(m.frames.size());
    const size_t pixels = size_t(m.w) * size_t(m.h);
    if (n < 2 || radius <= 0) return;
    const std::vector<std::vector<uint8_t>> raw = m.frames;
    for (int f = 0; f < n; ++f) {
        const int lo = std::max(0, f - radius), hi = std::min(n - 1, f + radius);
        std::vector<const uint8_t*> window;
        for (int k = lo; k <= hi; ++k) window.push_back(raw[size_t(k)].data());
        smoothFrame(window, f - lo, pixels, m.frames[size_t(f)].data(), tolerance);
    }
}

void growMatte(std::vector<uint8_t>& m, int w, int h, double px)
{
    if (!(std::abs(px) > 1e-3) || m.size() != size_t(w) * size_t(h)) return;
    const bool grow = px > 0;
    const double a = std::abs(px);
    const int r0 = int(a);
    const float f = float(a - r0);
    const std::vector<uint8_t> lo = extreme(m, w, h, r0, grow);
    if (f < 1e-3f) {
        m = lo;
        return;
    }
    const std::vector<uint8_t> hi = extreme(m, w, h, r0 + 1, grow);
    for (size_t i = 0; i < m.size(); ++i) m[i] = uint8_t(lo[i] + (hi[i] - lo[i]) * f + 0.5f);
}

void blurMatte(std::vector<uint8_t>& m, int w, int h, double px)
{
    const int r = int(std::lround(px));
    if (r <= 0 || m.size() != size_t(w) * size_t(h)) return;
    std::vector<float> a(m.begin(), m.end()), b;
    boxMean(a, b, w, h, r);
    boxMean(b, a, w, h, r);
    for (size_t i = 0; i < m.size(); ++i) m[i] = uint8_t(std::min(255.0f, a[i] + 0.5f));
}

void refineMatte(const uint8_t* low, int lw, int lh, const uint8_t* rgba, int w, int h, uint8_t* out, int radius,
                 float eps)
{
    if (!low || !out || lw <= 0 || lh <= 0 || w <= 0 || h <= 0) return;
    const std::vector<Sample> xs = samples(lw, w), ys = samples(lh, h);
    const size_t n = size_t(lw) * size_t(lh);
    if (!rgba) { // plain bilinear
        Compose::forRows(h, kMinRows, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const Sample sy = ys[size_t(y)];
                const uint8_t* r0 = low + size_t(sy.i0) * lw;
                const uint8_t* r1 = low + size_t(sy.i1) * lw;
                uint8_t* o = out + size_t(y) * w;
                for (int x = 0; x < w; ++x) {
                    const Sample sx = xs[size_t(x)];
                    const float top = r0[sx.i0] + (r0[sx.i1] - r0[sx.i0]) * sx.t;
                    const float bottom = r1[sx.i0] + (r1[sx.i1] - r1[sx.i0]) * sx.t;
                    o[x] = uint8_t(top + (bottom - top) * sy.t + 0.5f);
                }
            }
        });
        return;
    }
    constexpr float kLuma = 1.0f / (256.0f * 255.0f);
    // The guide on the small grid: the picture's luma averaged over the pixels each small pixel covers
    std::vector<int> colOf(static_cast<size_t>(w)), rowStart(static_cast<size_t>(lh) + 1);
    for (int x = 0; x < w; ++x) colOf[size_t(x)] = std::min(lw - 1, int(int64_t(x) * lw / w));
    for (int ly = 0; ly <= lh; ++ly) rowStart[size_t(ly)] = int((int64_t(ly) * h + lh - 1) / lh);
    std::vector<float> I(n, 0.0f);
    Compose::forRows(lh, 4, [&](int l0, int l1) {
        std::vector<int> count(static_cast<size_t>(lw));
        for (int ly = l0; ly < l1; ++ly) {
            std::fill(count.begin(), count.end(), 0);
            float* row = I.data() + size_t(ly) * lw;
            for (int y = rowStart[size_t(ly)]; y < rowStart[size_t(ly) + 1]; ++y) {
                const uint8_t* p = rgba + size_t(y) * w * 4;
                for (int x = 0; x < w; ++x, p += 4) {
                    row[colOf[size_t(x)]] += float(77 * p[0] + 150 * p[1] + 29 * p[2]) * kLuma;
                    ++count[size_t(colOf[size_t(x)])];
                }
            }
            for (int x = 0; x < lw; ++x) row[x] = count[size_t(x)] ? row[x] / float(count[size_t(x)]) : 0.0f;
        }
    });
    std::vector<float> p(n), II(n), Ip(n);
    for (size_t i = 0; i < n; ++i) {
        p[i] = low[i] * (1.0f / 255.0f);
        II[i] = I[i] * I[i];
        Ip[i] = I[i] * p[i];
    }
    std::vector<float> mI, mp, mII, mIp;
    boxMean(I, mI, lw, lh, radius);
    boxMean(p, mp, lw, lh, radius);
    boxMean(II, mII, lw, lh, radius);
    boxMean(Ip, mIp, lw, lh, radius);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float var = mII[i] - mI[i] * mI[i], cov = mIp[i] - mI[i] * mp[i];
        a[i] = cov / (var + eps);
        b[i] = mp[i] - a[i] * mI[i];
    }
    std::vector<float> A, B;
    boxMean(a, A, lw, lh, radius);
    boxMean(b, B, lw, lh, radius);
    // On the full picture: q = A * I + B, A and B bilinear
    Compose::forRows(h, kMinRows, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const Sample sy = ys[size_t(y)];
            const float* a0 = A.data() + size_t(sy.i0) * lw;
            const float* a1 = A.data() + size_t(sy.i1) * lw;
            const float* b0 = B.data() + size_t(sy.i0) * lw;
            const float* b1 = B.data() + size_t(sy.i1) * lw;
            const uint8_t* px = rgba + size_t(y) * w * 4;
            uint8_t* o = out + size_t(y) * w;
            for (int x = 0; x < w; ++x, px += 4) {
                const Sample sx = xs[size_t(x)];
                const float at = a0[sx.i0] + (a0[sx.i1] - a0[sx.i0]) * sx.t;
                const float ab = a1[sx.i0] + (a1[sx.i1] - a1[sx.i0]) * sx.t;
                const float bt = b0[sx.i0] + (b0[sx.i1] - b0[sx.i0]) * sx.t;
                const float bb = b1[sx.i0] + (b1[sx.i1] - b1[sx.i0]) * sx.t;
                const float luma = float(77 * px[0] + 150 * px[1] + 29 * px[2]) * kLuma;
                o[x] = byteOf((at + (ab - at) * sy.t) * luma + bt + (bb - bt) * sy.t);
            }
        }
    });
}

// ---------- Cache file ----------

QString cutoutCacheDir() { return AppPaths::writable(QStandardPaths::CacheLocation) + "/cutout"; }

QString cutoutCachePath(const QString& id) { return QDir(cutoutCacheDir()).filePath(id + ".matte"); }

bool writeMattes(const QString& path, int w, int h, int from, const std::vector<QByteArray>& frames, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    QDataStream s(&f);
    s.setVersion(QDataStream::Qt_6_0);
    s << kMagic << kFileVersion << qint32(w) << qint32(h) << qint32(from) << qint32(frames.size());
    for (const QByteArray& b : frames) s << b;
    if (s.status() != QDataStream::Ok || !f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

bool writeMattes(const QString& path, const MatteSeq& m, QString* error)
{
    std::vector<QByteArray> frames;
    for (const auto& f : m.frames) frames.push_back(qCompress(f.data(), qsizetype(f.size()), 6));
    return writeMattes(path, m.w, m.h, m.from, frames, error);
}

std::shared_ptr<const MatteFile> MatteFile::load(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("no cutout analysis %1").arg(path);
        return nullptr;
    }
    QDataStream s(&f);
    s.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0;
    qint32 w = 0, h = 0, from = 0, count = 0;
    s >> magic >> version >> w >> h >> from >> count;
    if (s.status() != QDataStream::Ok || magic != kMagic || version != kFileVersion || w <= 0 || h <= 0 || count < 0
        || int64_t(w) * h > 16 * 1024 * 1024) {
        if (error) *error = QStringLiteral("%1 is no cutout analysis of this version").arg(path);
        return nullptr;
    }
    auto m = std::make_shared<MatteFile>();
    m->w = w, m->h = h, m->from = from;
    m->m_frames.reserve(size_t(std::min(count, 1 << 20)));
    for (qint32 i = 0; i < count; ++i) {
        QByteArray b;
        s >> b;
        if (s.status() != QDataStream::Ok) {
            if (error) *error = QStringLiteral("%1 is cut short").arg(path);
            return nullptr;
        }
        m->m_frames.push_back(std::move(b));
    }
    return m;
}

std::vector<uint8_t> MatteFile::matte(int frame) const
{
    if (!has(frame)) return {};
    const QByteArray raw = qUncompress(m_frames[size_t(frame - from)]);
    if (raw.size() != qsizetype(w) * h) return {};
    return std::vector<uint8_t>(raw.begin(), raw.end());
}

std::optional<MatteSeq> readMattes(const QString& path, QString* error)
{
    const auto f = MatteFile::load(path, error);
    if (!f) return std::nullopt;
    MatteSeq m;
    m.w = f->w, m.h = f->h, m.from = f->from;
    for (int i = 0; i < f->count(); ++i) {
        std::vector<uint8_t> one = f->matte(f->from + i);
        if (one.empty()) {
            if (error) *error = QStringLiteral("%1: frame %2 is broken").arg(path).arg(f->from + i);
            return std::nullopt;
        }
        m.frames.push_back(std::move(one));
    }
    return m;
}

namespace {

struct Loaded {
    qint64 size = -1, mtime = 0;
    std::shared_ptr<const MatteFile> file;
};
QMutex g_loadedMutex;
QHash<QString, Loaded> g_loaded; // a few analyses in memory (compressed), by file
constexpr int kKeepLoaded = 8;

} // namespace

std::shared_ptr<const MatteFile> mattesOf(const QString& id)
{
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || id.contains("..")) return nullptr;
    const QString path = cutoutCachePath(id);
    const QFileInfo info(path);
    if (!info.isFile()) return nullptr;
    const qint64 size = info.size(), mtime = info.lastModified().toMSecsSinceEpoch();
    {
        QMutexLocker lock(&g_loadedMutex);
        const auto it = g_loaded.constFind(path);
        if (it != g_loaded.constEnd() && it->size == size && it->mtime == mtime) return it->file;
    }
    auto file = MatteFile::load(path);
    QMutexLocker lock(&g_loadedMutex);
    if (g_loaded.size() >= kKeepLoaded && !g_loaded.contains(path)) g_loaded.erase(g_loaded.begin());
    g_loaded.insert(path, Loaded{size, mtime, file});
    return file;
}

bool removeCutoutCache(const QString& id)
{
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || id.contains("..")) return false;
    const QString path = cutoutCachePath(id);
    {
        QMutexLocker lock(&g_loadedMutex);
        g_loaded.remove(path);
    }
    return !QFileInfo::exists(path) || QFile::remove(path);
}

// ---------- Analysis ----------

QString cutoutCacheId(const Clip& c, int node, int from, int to, const QString& way)
{
    const QString key = QString("v%1|%2|%3|%4|%5|%6|%7|%8|%9")
                            .arg(kFileVersion)
                            .arg(c.mediaPath)
                            .arg(c.in)
                            .arg(c.out)
                            .arg(c.id)
                            .arg(node)
                            .arg(from)
                            .arg(to)
                            .arg(way);
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(24));
}

CutoutRun cutoutFrames(const std::function<QImage(int)>& frame, int from, int to, const PersonMatte::Segmenter& s,
                       const QString& id, const std::function<void(int, int)>& progress,
                       const std::atomic<bool>* cancel)
{
    CutoutRun run;
    run.cacheId = id;
    if (to < from) std::swap(from, to);
    run.from = from, run.to = to;
    const int total = to - from + 1;
    QElapsedTimer timer;
    timer.start();
    int lw = 0, lh = 0;
    std::vector<std::vector<uint8_t>> raw(static_cast<size_t>(total));
    std::vector<QByteArray> packed;
    packed.reserve(size_t(total));
    std::vector<uint8_t> smoothed;
    // Smoothed and packed as soon as the frames after it are there; older raw frames are let go
    const auto emitUpTo = [&](int last) {
        while (int(packed.size()) <= last) {
            const int e = int(packed.size());
            const int lo = std::max(0, e - kSmoothRadius), hi = std::min(total - 1, e + kSmoothRadius);
            std::vector<const uint8_t*> window;
            for (int k = lo; k <= hi; ++k) window.push_back(raw[size_t(k)].data());
            smoothed.resize(size_t(lw) * lh);
            smoothFrame(window, e - lo, smoothed.size(), smoothed.data());
            packed.push_back(qCompress(smoothed.data(), qsizetype(smoothed.size()), 6));
            if (e - kSmoothRadius >= 0) std::vector<uint8_t>().swap(raw[size_t(e - kSmoothRadius)]);
        }
    };
    for (int i = 0; i < total; ++i) {
        if (cancel && cancel->load()) {
            run.canceled = true;
            return run;
        }
        const QImage img = frame(from + i);
        if (img.isNull()) {
            run.error = T("Kein Bild vom Clip");
            return run;
        }
        if (i == 0) {
            const QSize ms = matteSize(img.size());
            lw = ms.width(), lh = ms.height();
        }
        int mw = 0, mh = 0;
        QString err;
        const std::vector<uint8_t> plane = PersonMatte::segment(s, img, &mw, &mh, &err);
        if (plane.empty()) {
            run.error = err.isEmpty() ? PersonMatte::needsExtension() : err;
            return run;
        }
        raw[size_t(i)] = resizePlane(plane.data(), mw, mh, lw, lh);
        emitUpTo(i - kSmoothRadius);
        if (progress) progress(i + 1, total);
    }
    emitUpTo(total - 1);
    QString err;
    if (!writeMattes(cutoutCachePath(id), lw, lh, from, packed, &err)) {
        run.error = err;
        return run;
    }
    run.frames = total;
    run.msPerFrame = double(timer.nsecsElapsed()) / 1e6 / std::max(1, total);
    return run;
}

CutoutRun cutoutNode(const ProjectFormat& format, const Clip& c, int node, int from, int to,
                     const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel,
                     const PersonMatte::Segmenter* given)
{
    CutoutRun run;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    CompGraph g;
    if (!e || !::Comp::graphOf(*e, &g) || !g.node(node) || g.node(node)->type != QLatin1String(kCutoutType)) {
        run.error = T("Der Clip hat keinen Freistell-Knoten %1").arg(node);
        return run;
    }
    if (c.isNested() || c.isTitle()) {
        run.error = T("Freistellen geht nur bei Video-Clips (nicht bei Titeln, Verbund- und Multicam-Clips)");
        return run;
    }
    std::optional<PersonMatte::Segmenter> own;
    if (!given) {
        QString err;
        own = PersonMatte::open(&err);
        if (!own) {
            run.error = err;
            return run;
        }
        given = &*own;
    }
    from = std::clamp(from, 0, c.length() - 1);
    to = std::clamp(to, 0, c.length() - 1);
    if (to < from) std::swap(from, to);
    const auto source = Tracker::clipPictures(format, c);
    return cutoutFrames([&](int f) { return source(f, kAnalysisEdge); }, from, to, *given,
                        cutoutCacheId(c, node, from, to, given->way), progress, cancel);
}

QString cutoutHint(const QString& cacheId)
{
    if (cacheId.isEmpty())
        return T("Noch nicht analysiert: erst „Freistellen analysieren“ ausführen (CLI: cutout_node)");
    if (!mattesOf(cacheId)) return T("Die Freistell-Analyse fehlt (Cache gelöscht): bitte neu analysieren");
    return {};
}

} // namespace Picture::Comp
