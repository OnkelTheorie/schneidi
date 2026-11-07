// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/PersonMatte.h"

#include "core/I18n.h"
#include "engine/Extensions.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>

#include <algorithm>
#include <cmath>
#include <memory>

namespace PersonMatte {

namespace {

QMutex g_standInMutex;
std::optional<Segmenter> g_standIn;

uint8_t toByte(float v)
{
    const float b = v * 255.0f + 0.5f;
    return uint8_t(b <= 0.0f ? 0 : b >= 255.0f ? 255 : int(b));
}

} // namespace

Inference::Tensor input(const QImage& img, const Spec& spec)
{
    Inference::Tensor t;
    const int W = std::max(1, spec.width), H = std::max(1, spec.height);
    t.shape = spec.nhwc ? std::vector<int64_t>{1, H, W, 3} : std::vector<int64_t>{1, 3, H, W};
    t.data.assign(size_t(3) * W * H, (0.0f - spec.mean) / spec.std);
    if (img.isNull()) return t;
    const QImage src = img.format() == QImage::Format_RGB32 || img.format() == QImage::Format_ARGB32
                           ? img
                           : img.convertToFormat(QImage::Format_RGB32);
    const int sw = src.width(), sh = src.height();
    const float sx = float(sw) / W, sy = float(sh) / H;
    const float scale = 1.0f / (255.0f * spec.std), offset = -spec.mean / spec.std;
    const size_t plane = size_t(W) * H;
    for (int y = 0; y < H; ++y) {
        const float fy = std::clamp((y + 0.5f) * sy - 0.5f, 0.0f, float(sh - 1));
        const int y0 = int(fy), y1 = std::min(sh - 1, y0 + 1);
        const float wy = fy - y0;
        const QRgb* r0 = reinterpret_cast<const QRgb*>(src.constScanLine(y0));
        const QRgb* r1 = reinterpret_cast<const QRgb*>(src.constScanLine(y1));
        for (int x = 0; x < W; ++x) {
            const float fx = std::clamp((x + 0.5f) * sx - 0.5f, 0.0f, float(sw - 1));
            const int x0 = int(fx), x1 = std::min(sw - 1, x0 + 1);
            const float wx = fx - x0;
            const QRgb a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
            const auto mix = [&](int (*ch)(QRgb)) {
                const float top = ch(a) + (ch(b) - ch(a)) * wx, bottom = ch(c) + (ch(d) - ch(c)) * wx;
                return (top + (bottom - top) * wy) * scale + offset;
            };
            const float rgb[3] = {mix(qRed), mix(qGreen), mix(qBlue)};
            const size_t at = size_t(y) * W + size_t(x);
            for (int ch = 0; ch < 3; ++ch) {
                if (spec.nhwc) t.data[at * 3 + size_t(ch)] = rgb[ch];
                else t.data[size_t(ch) * plane + at] = rgb[ch];
            }
        }
    }
    return t;
}

std::vector<uint8_t> alpha(const Inference::Tensor& out, int* w, int* h, QString* error)
{
    const auto& s = out.shape;
    int channels = 0, H = 0, W = 0;
    bool nhwc = false;
    // NCHW unless the last axis is the short one (1 x H x W x C)
    if (s.size() == 4 && s[0] == 1 && (s[1] == 1 || s[1] == 2) && s[3] > 2) {
        channels = int(s[1]), H = int(s[2]), W = int(s[3]);
    } else if (s.size() == 4 && s[0] == 1 && (s[3] == 1 || s[3] == 2)) {
        channels = int(s[3]), H = int(s[1]), W = int(s[2]), nhwc = true;
    } else if (s.size() == 3 && s[0] == 1) { // 1 x H x W: one channel
        channels = 1, H = int(s[1]), W = int(s[2]);
    }
    const size_t n = size_t(std::max(0, W)) * size_t(std::max(0, H));
    if (channels == 0 || n == 0 || out.data.size() != n * size_t(channels)) {
        if (error) *error = QStringLiteral("the cutout model answered with an unknown shape");
        return {};
    }
    std::vector<uint8_t> plane(n);
    if (channels == 1) {
        const auto [lo, hi] = std::minmax_element(out.data.begin(), out.data.end());
        const bool probability = *lo >= 0.0f && *hi <= 1.0f;
        for (size_t i = 0; i < n; ++i) {
            const float v = out.data[i];
            plane[i] = toByte(probability ? v : 1.0f / (1.0f + std::exp(-v)));
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            const float bg = nhwc ? out.data[2 * i] : out.data[i];
            const float fg = nhwc ? out.data[2 * i + 1] : out.data[n + i];
            // softmax of two = sigmoid of the difference
            plane[i] = toByte(1.0f / (1.0f + std::exp(bg - fg)));
        }
    }
    if (w) *w = W;
    if (h) *h = H;
    if (error) error->clear();
    return plane;
}

std::vector<uint8_t> segment(const Segmenter& s, const QImage& img, int* w, int* h, QString* error)
{
    if (!s.run) {
        if (error) *error = needsExtension();
        return {};
    }
    QString e;
    std::optional<Inference::Tensor> out = s.run(input(img, s.spec), &e);
    if (!out) {
        if (error) *error = e.isEmpty() ? QStringLiteral("the cutout model failed") : e;
        return {};
    }
    return alpha(*out, w, h, error);
}

QString needsExtension()
{
    return T("Freistellen braucht die Erweiterung „Freistellen (Personen)“ (Erweiterungen…)");
}

bool available(QString* why)
{
    {
        QMutexLocker lock(&g_standInMutex);
        if (g_standIn) return true;
    }
    if (Extensions::cutoutModel().isEmpty()) {
        if (why) *why = QStringLiteral("the cutout model is not installed (extension \"cutout\")");
        return false;
    }
    return Inference::available(why);
}

void setStandIn(std::optional<Segmenter> s)
{
    QMutexLocker lock(&g_standInMutex);
    g_standIn = std::move(s);
}

std::optional<Segmenter> open(QString* error)
{
    {
        QMutexLocker lock(&g_standInMutex);
        if (g_standIn) return g_standIn;
    }
    const QString model = Extensions::cutoutModel();
    if (model.isEmpty()) {
        if (error) *error = needsExtension();
        return std::nullopt;
    }
    QString why;
    if (!Inference::available(&why)) {
        if (error) *error = needsExtension() + QStringLiteral(" (%1)").arg(why);
        return std::nullopt;
    }
    return openModel(model, error);
}

std::optional<Segmenter> openModel(const QString& model, QString* error)
{
    std::shared_ptr<Inference::Session> session = Inference::Session::open(model, error);
    if (!session) return std::nullopt;
    const auto& in = session->inputs();
    if (in.size() != 1 || in[0].shape.size() != 4 || !in[0].isFloat || session->outputs().empty()) {
        if (error) *error = model + " is no portrait segmentation model (one RGB picture in, a matte out)";
        return std::nullopt;
    }
    Segmenter s;
    const auto& d = in[0].shape;
    s.spec.nhwc = d[3] == 3 && d[1] != 3;
    const int64_t H = s.spec.nhwc ? d[1] : d[2], W = s.spec.nhwc ? d[2] : d[3];
    s.spec.height = H > 0 ? int(H) : 192; // dynamic: the size the catalog's model was made for
    s.spec.width = W > 0 ? int(W) : 192;
    const QFileInfo m(model);
    s.way = QString("ppseg|%1|%2|%3").arg(m.absoluteFilePath()).arg(m.size()).arg(m.lastModified().toMSecsSinceEpoch());
    auto lock = std::make_shared<QMutex>();
    s.run = [session, lock](const Inference::Tensor& t, QString* err) -> std::optional<Inference::Tensor> {
        QMutexLocker l(lock.get()); // a Session is used by one thread at a time
        auto outs = session->run({t}, err);
        if (!outs || outs->empty()) return std::nullopt;
        return std::move(outs->front());
    };
    if (error) error->clear();
    return s;
}

} // namespace PersonMatte
