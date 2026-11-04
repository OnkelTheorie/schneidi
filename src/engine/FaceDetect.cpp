// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/FaceDetect.h"

#include "engine/Extensions.h"
#include "engine/Inference.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMutex>

#include <algorithm>
#include <cmath>

namespace FaceDetect {

std::vector<Face> decode(const std::vector<Level>& levels, int inputWidth, int inputHeight, float minScore)
{
    std::vector<Face> out;
    for (const Level& l : levels) {
        if (l.stride <= 0) continue;
        const int cols = inputWidth / l.stride, rows = inputHeight / l.stride;
        const size_t cells = size_t(cols) * size_t(rows);
        if (l.cls.size() < cells || l.obj.size() < cells || l.bbox.size() < 4 * cells) continue; // not this size
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const size_t i = size_t(r) * size_t(cols) + size_t(c);
                // As FaceDetectorYN (2023mar): both scores clamped to 0..1, their geometric mean
                const float cls = std::clamp(l.cls[i], 0.0f, 1.0f), obj = std::clamp(l.obj[i], 0.0f, 1.0f);
                const float score = std::sqrt(cls * obj);
                if (!(score >= minScore)) continue;
                const float* b = l.bbox.data() + 4 * i;
                const double cx = (c + b[0]) * l.stride, cy = (r + b[1]) * l.stride;
                const double w = std::exp(double(b[2])) * l.stride, h = std::exp(double(b[3])) * l.stride;
                if (!std::isfinite(w) || !std::isfinite(h)) continue;
                out.push_back(Face{cx - w / 2, cy - h / 2, w, h, score});
            }
    }
    return out;
}

double overlap(const Face& a, const Face& b)
{
    const double ix = std::max(0.0, std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x));
    const double iy = std::max(0.0, std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y));
    const double inter = ix * iy, uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0 ? inter / uni : 0.0;
}

std::vector<Face> suppress(std::vector<Face> faces, float maxOverlap)
{
    std::stable_sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
    std::vector<Face> keep;
    for (const Face& f : faces) {
        bool covered = false;
        for (const Face& k : keep) covered |= overlap(f, k) > maxOverlap;
        if (!covered) keep.push_back(f);
    }
    return keep;
}

bool available(QString* why)
{
    if (Extensions::faceModel().isEmpty()) {
        if (why) *why = "the face model is not installed (extension \"faces\")";
        return false;
    }
    return Inference::available(why);
}

QString wayKey(const QString& model)
{
    if (model.isEmpty()) return {};
    const QFileInfo m(model);
    return QString("yunet|%1|%2|%3").arg(m.absoluteFilePath()).arg(m.size()).arg(m.lastModified().toMSecsSinceEpoch());
}

QString way()
{
    // Once per process and model file (the index asks on every cache lookup): whether the runtime loads is asked only
    // when the model file is new or changed (as AudioEvents::way)
    const QString model = Extensions::faceModel();
    if (model.isEmpty()) return {};
    const QString stat = wayKey(model);
    static QMutex mutex;
    static QString lastStat, lastWay;
    {
        QMutexLocker lock(&mutex);
        if (stat == lastStat) return lastWay;
    }
    const QString w = available() ? stat : QString();
    QMutexLocker lock(&mutex);
    lastStat = stat;
    lastWay = w;
    return w;
}

// ---------- Detector ----------

Detector::Detector() = default;
Detector::~Detector() = default;

std::unique_ptr<Detector> Detector::open(QString* error)
{
    const QString model = Extensions::faceModel();
    if (model.isEmpty()) {
        if (error) *error = "the face model is not installed (extension \"faces\")";
        return nullptr;
    }
    return openModel(model, error);
}

std::unique_ptr<Detector> Detector::openModel(const QString& model, QString* error)
{
    auto session = Inference::Session::open(model, error);
    if (!session) return nullptr;
    const auto& in = session->inputs();
    const auto& out = session->outputs();
    // One picture input [1, 3, H, W] (fixed 640 x 640, or dynamic), at least cls/obj/bbox of the three strides
    if (in.size() != 1 || in[0].shape.size() != 4 || !in[0].isFloat || out.size() < 9
        || (in[0].shape[2] > 0 && in[0].shape[2] != kInput) || (in[0].shape[3] > 0 && in[0].shape[3] != kInput)) {
        if (error) *error = model + " is no YuNet 2023mar face model (one 640 x 640 picture in, cls/obj/bbox out)";
        return nullptr;
    }
    std::unique_ptr<Detector> d(new Detector);
    for (int s = 0; s < 3; ++s)
        for (int i = 0; i < int(out.size()); ++i) {
            const QString& name = out[size_t(i)].name;
            if (name == QString("cls_%1").arg(kStrides[s])) d->m_cls[s] = i;
            if (name == QString("obj_%1").arg(kStrides[s])) d->m_obj[s] = i;
            if (name == QString("bbox_%1").arg(kStrides[s])) d->m_bbox[s] = i;
        }
    d->m_session = std::move(session);
    return d;
}

std::optional<std::vector<Face>> Detector::detect(const uint8_t* bgr, int width, int height, int stride,
                                                  QString* error, float minScore)
{
    if (!bgr || width <= 0 || height <= 0) {
        if (error) *error = "no picture";
        return std::nullopt;
    }
    // The picture scaled to 640 on its longer side (bilinear), top-left; the rest black
    const double scale = double(kInput) / std::max(width, height);
    const int sw = std::clamp(int(std::lround(width * scale)), 1, kInput);
    const int sh = std::clamp(int(std::lround(height * scale)), 1, kInput);
    Inference::Tensor t;
    t.shape = {1, 3, kInput, kInput};
    t.data.assign(size_t(3) * kInput * kInput, 0.0f);
    const size_t plane = size_t(kInput) * kInput;
    for (int y = 0; y < sh; ++y) {
        const double fy = std::clamp((y + 0.5) / scale - 0.5, 0.0, double(height - 1));
        const int y0 = int(fy), y1 = std::min(height - 1, y0 + 1);
        const float wy = float(fy - y0);
        const uint8_t* r0 = bgr + size_t(y0) * size_t(stride);
        const uint8_t* r1 = bgr + size_t(y1) * size_t(stride);
        for (int x = 0; x < sw; ++x) {
            const double fx = std::clamp((x + 0.5) / scale - 0.5, 0.0, double(width - 1));
            const int x0 = int(fx), x1 = std::min(width - 1, x0 + 1);
            const float wx = float(fx - x0);
            const size_t at = size_t(y) * kInput + size_t(x);
            for (int ch = 0; ch < 3; ++ch) {
                const float top = r0[3 * x0 + ch] + (r0[3 * x1 + ch] - r0[3 * x0 + ch]) * wx;
                const float bottom = r1[3 * x0 + ch] + (r1[3 * x1 + ch] - r1[3 * x0 + ch]) * wx;
                t.data[size_t(ch) * plane + at] = top + (bottom - top) * wy;
            }
        }
    }
    auto outs = m_session->run({t}, error);
    if (!outs) return std::nullopt;
    std::vector<Level> levels(3);
    for (int s = 0; s < 3; ++s) {
        levels[size_t(s)].stride = kStrides[s];
        levels[size_t(s)].cls = std::move((*outs)[size_t(m_cls[s])].data);
        levels[size_t(s)].obj = std::move((*outs)[size_t(m_obj[s])].data);
        levels[size_t(s)].bbox = std::move((*outs)[size_t(m_bbox[s])].data);
    }
    std::vector<Face> faces = suppress(decode(levels, kInput, kInput, minScore));
    // To fractions of the picture (the part of the input it covers), clipped to it
    std::vector<Face> out;
    for (Face f : faces) {
        const double x0 = std::clamp(f.x / sw, 0.0, 1.0), y0 = std::clamp(f.y / sh, 0.0, 1.0);
        const double x1 = std::clamp((f.x + f.w) / sw, 0.0, 1.0), y1 = std::clamp((f.y + f.h) / sh, 0.0, 1.0);
        if (x1 <= x0 || y1 <= y0) continue; // in the black padding
        out.push_back(Face{x0, y0, x1 - x0, y1 - y0, f.score});
    }
    if (error) error->clear();
    return out;
}

std::vector<uint8_t> bgrFromYuv420(const uint8_t* const planes[3], const int linesize[3], int width, int height,
                                   bool fullRange)
{
    std::vector<uint8_t> out(size_t(std::max(0, width)) * size_t(std::max(0, height)) * 3);
    // BT.601 (the detector cares little for the exact matrix)
    const double ys = fullRange ? 1.0 : 255.0 / 219.0, cs = fullRange ? 1.0 : 255.0 / 224.0, y0 = fullRange ? 0 : 16;
    for (int y = 0; y < height; ++y) {
        const uint8_t* l = planes[0] + size_t(y) * size_t(linesize[0]);
        const uint8_t* u = planes[1] + size_t(y / 2) * size_t(linesize[1]);
        const uint8_t* v = planes[2] + size_t(y / 2) * size_t(linesize[2]);
        uint8_t* o = out.data() + size_t(y) * size_t(width) * 3;
        for (int x = 0; x < width; ++x) {
            const double Y = (l[x] - y0) * ys, U = (u[x / 2] - 128) * cs, V = (v[x / 2] - 128) * cs;
            const auto px = [](double c) { return uint8_t(std::clamp(int(std::lround(c)), 0, 255)); };
            o[3 * x + 0] = px(Y + 1.772 * U);
            o[3 * x + 1] = px(Y - 0.344136 * U - 0.714136 * V);
            o[3 * x + 2] = px(Y + 1.402 * V);
        }
    }
    return out;
}

} // namespace FaceDetect
