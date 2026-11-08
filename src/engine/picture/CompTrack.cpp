// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/CompTrack.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "engine/Tracker.h"
#include "engine/picture/CompNodes.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace Picture::Comp {

namespace {

constexpr double kTemplateMax = 48; // longest template side in the working picture (speed; like the window tracker)
constexpr int kFaceEdge = 640;      // the face model looks at 640 x 640 anyway
constexpr int kMaxWiden = 4;        // search radius after a loss grows up to this many times the normal one

QString needsFaces()
{
    return T("Gesichter verfolgen braucht die Erweiterung „Gesichtserkennung (YuNet)“ (Erweiterungen…)");
}

double distance(const FaceDetect::Face& f, double x, double y, double aspect)
{
    // In picture widths: y fractions count `aspect` (height / width) as much
    return std::hypot(f.x + f.w / 2 - x, (f.y + f.h / 2 - y) * aspect);
}

TrackRun trackFaces(const std::function<QImage(int)>& frame, const QImage& first, const TrackStart& st, int from,
                    int to, const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel,
                    const FaceFinder& given)
{
    TrackRun run;
    QString err;
    const FaceFinder find = given ? given : installedFaces(&err);
    if (!find) {
        run.error = err;
        return run;
    }
    const double aspect = double(first.height()) / std::max(1, first.width());
    const auto detect = [&](const QImage& img, bool* failed) {
        const QImage small = std::max(img.width(), img.height()) > kFaceEdge
                                 ? img.scaled(kFaceEdge, kFaceEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                 : img;
        QString e;
        std::optional<std::vector<FaceDetect::Face>> faces = find(small, &e);
        *failed = !faces;
        if (!faces) run.error = e.isEmpty() ? needsFaces() : e;
        return faces.value_or(std::vector<FaceDetect::Face>{});
    };
    bool failed = false;
    std::vector<FaceDetect::Face> faces = detect(first, &failed);
    if (failed) return run;
    // The face at the start point: one around it, else the nearest within half a picture width
    const FaceDetect::Face* pick = nullptr;
    double best = 0.5;
    for (const FaceDetect::Face& f : faces) {
        const bool inside = st.a.x() >= f.x && st.a.x() <= f.x + f.w && st.a.y() >= f.y && st.a.y() <= f.y + f.h;
        const double d = inside ? 0 : distance(f, st.a.x(), st.a.y(), aspect);
        if (d < best || (inside && d <= best)) {
            best = d;
            pick = &f;
        }
    }
    if (!pick) {
        run.error = T("Kein Gesicht am Startpunkt");
        return run;
    }
    FaceDetect::Face last = *pick;
    const double w0 = std::max(1e-6, last.w);
    const auto point = [&](int f) {
        return ::Comp::TrackPoint{f, last.x + last.w / 2, last.y + last.h / 2, last.w / w0, 0};
    };
    run.points << point(from);
    const int step = to > from ? 1 : -1, total = std::abs(to - from);
    int lostRun = 0;
    for (int f = from + step, done = 1; done <= total; f += step, ++done) {
        if (cancel && cancel->load()) {
            run.canceled = true;
            break;
        }
        const QImage img = frame(f);
        bool found = false;
        if (!img.isNull()) {
            faces = detect(img, &failed);
            if (failed) break;
            // The same face: the nearest one, not further than its own width (wider while it was not seen)
            const double reach = last.w * (1 + 0.5 * std::min(lostRun, 2 * kMaxWiden));
            const FaceDetect::Face* next = nullptr;
            double nearest = reach;
            for (const FaceDetect::Face& c : faces)
                if (const double d = distance(c, last.x + last.w / 2, last.y + last.h / 2, aspect); d <= nearest) {
                    nearest = d;
                    next = &c;
                }
            if (next) {
                last = *next;
                found = true;
            }
        }
        if (found) {
            if (lostRun && run.foundAt < 0) run.foundAt = f;
            lostRun = 0;
            run.points << point(f);
        } else {
            run.lostFrames << f;
            if (run.lostAt < 0) run.lostAt = f;
            if (++lostRun >= st.maxLost) {
                run.gaveUp = true;
                break;
            }
        }
        if (progress) progress(done, total);
    }
    return run;
}

} // namespace

bool trackModeFromId(const QString& id, TrackMode* mode)
{
    TrackMode m;
    if (id == QLatin1String("point")) m = TrackMode::Point;
    else if (id == QLatin1String("two")) m = TrackMode::Two;
    else if (id == QLatin1String("face")) m = TrackMode::Face;
    else return false;
    if (mode) *mode = m;
    return true;
}

FaceFinder installedFaces(QString* error)
{
    QString why;
    std::unique_ptr<FaceDetect::Detector> det;
    if (FaceDetect::available(&why)) det = FaceDetect::Detector::open(&why);
    if (!det) {
        if (error) *error = needsFaces() + (why.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(why));
        return {};
    }
    std::shared_ptr<FaceDetect::Detector> shared(std::move(det));
    return [shared](const QImage& img, QString* err) {
        const QImage bgr = img.convertToFormat(QImage::Format_BGR888);
        return shared->detect(bgr.constBits(), bgr.width(), bgr.height(), int(bgr.bytesPerLine()), err);
    };
}

TrackRun trackFrames(const std::function<QImage(int)>& frame, const TrackStart& st, int from, int to,
                     const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel,
                     const FaceFinder& faces)
{
    TrackRun run;
    const QImage first = frame ? frame(from) : QImage();
    if (first.isNull()) {
        run.error = T("Kein Bild vom Clip");
        return run;
    }
    if (st.mode == TrackMode::Face) return trackFaces(frame, first, st, from, to, progress, cancel, faces);

    // Working picture: small enough that the template has at most kTemplateMax pixels a side
    const QSize full = first.size();
    const double tpl = std::max(4.0, st.size * full.width());
    const double s = std::min(1.0, kTemplateMax / tpl);
    const QSize work(std::max(1, int(std::lround(full.width() * s))), std::max(1, int(std::lround(full.height() * s))));
    const auto gray = [&](const QImage& img) {
        return Tracker::toColor(img.size() == work ? img
                                                   : img.scaled(work, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    };
    const double side = tpl * s;
    const double W = work.width(), H = work.height();
    const auto region = [&](QPointF p) { return QRectF(p.x() * W - side / 2, p.y() * H - side / 2, side, side); };
    const Tracker::Gray g0 = gray(first);

    std::optional<Tracker::PointTracker> one;
    std::optional<Tracker::TwoPointTracker> two;
    QPointF origin, start = st.a; // tracked position at the start, its place 0..1
    if (st.mode == TrackMode::Two) {
        two.emplace(g0, region(st.a), region(st.b));
        if (!two->valid()) {
            run.error = T("Die zwei Punkte liegen aufeinander");
            return run;
        }
        two->setAdaptWhenUnsure(false);
        origin = two->center();
        start = (st.a + st.b) / 2;
    } else {
        one.emplace(g0, region(st.a));
        if (!one->valid()) {
            run.error = T("Kein Bild vom Clip");
            return run;
        }
        one->setAdaptWhenUnsure(false);
        origin = one->center();
    }
    const auto point = [&](int f) {
        const QPointF d = (two ? two->center() : one->center()) - origin;
        return ::Comp::TrackPoint{f, start.x() + d.x() / W, start.y() + d.y() / H, two ? two->scale() : 1.0,
                                  two ? two->angle() : 0.0};
    };
    run.points << point(from);
    const int normal = std::max(8, int(0.6 * side));
    const int step = to > from ? 1 : -1, total = std::abs(to - from);
    int lostRun = 0;
    for (int f = from + step, done = 1; done <= total; f += step, ++done) {
        if (cancel && cancel->load()) {
            run.canceled = true;
            break;
        }
        const QImage img = frame(f);
        bool found = false;
        if (!img.isNull()) {
            // After a loss the thing moved on behind what hid it: look wider every frame
            const int radius = lostRun ? normal * std::min(kMaxWiden, 1 + lostRun) : 0;
            const Tracker::Gray g = gray(img);
            found = two ? two->next(g, radius) : one->next(g, radius);
            if (found && two && two->outlier()) ++run.outliers;
        }
        if (found) {
            if (lostRun && run.foundAt < 0) run.foundAt = f;
            lostRun = 0;
            run.points << point(f);
        } else {
            run.lostFrames << f;
            if (run.lostAt < 0) run.lostAt = f;
            if (++lostRun >= st.maxLost) {
                run.gaveUp = true;
                break;
            }
        }
        if (progress) progress(done, total);
    }
    return run;
}

TrackStart startOf(const QVariantMap& values)
{
    TrackStart st;
    const NodeType* k = find(::Comp::kTrackerType);
    if (!k) return st;
    trackModeFromId(textValue(*k, values, "mode"), &st.mode);
    st.a = QPointF(numValue(*k, values, "ax"), numValue(*k, values, "ay"));
    st.b = QPointF(numValue(*k, values, "bx"), numValue(*k, values, "by"));
    st.size = std::clamp(numValue(*k, values, "size"), 0.005, 1.0);
    return st;
}

double trackerValue(const QVariantMap& values, const QString& key)
{
    const NodeType* k = find(::Comp::kTrackerType);
    if (!k) return values.value(key).toDouble();
    return ::Comp::followValue(
        key, textValue(*k, values, QStringLiteral("mode")), [&](const QString& q) { return values.contains(q); },
        [&](const QString& q) { return numValue(*k, values, q); });
}

TrackRun trackNode(const ProjectFormat& format, const Clip& c, int node, int from, int to,
                   const std::function<void(int, int)>& progress, const std::atomic<bool>* cancel,
                   const FaceFinder& faces)
{
    TrackRun run;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    CompGraph g;
    const NodeType* k = find(::Comp::kTrackerType);
    if (!k || !e || !::Comp::graphOf(*e, &g) || !g.node(node) || g.node(node)->type != QLatin1String(::Comp::kTrackerType)) {
        run.error = T("Der Clip hat keinen Verfolgungsknoten %1").arg(node);
        return run;
    }
    if (c.isNested() || c.isTitle()) {
        run.error = T("Verfolgung geht nur bei Video-Clips (nicht bei Titeln, Verbund- und Multikamera-Clips)");
        return run;
    }
    from = std::clamp(from, 0, c.length() - 1);
    to = std::clamp(to, 0, c.length() - 1);
    // The node's values at the start frame (start points may be keyframed: placed anew where tracking restarts)
    QVariantMap values;
    for (const EffectParam& p : k->params) {
        const QString full = CompGraph::paramKey(node, p.key);
        if (p.type == EffectParam::Double) {
            const AnimParam ap = Keys::effectParam(::Comp::kEffectId, full);
            values[p.key] = Keys::animated(c, ap) ? QVariant(Keys::valueAt(c, ap, from)) : e->params.value(full, p.defaultValue);
        } else if (e->params.contains(full)) {
            values[p.key] = e->params.value(full);
        }
    }
    const TrackStart st = startOf(values);
    // Pictures as big as the working picture needs (a small template = a big picture, at most the project size).
    // Tracked on the clip's own picture without bars (a portrait clip in a landscape project): the graph works on
    // that one, so ax/ay and the results are 0..1 of it. The edge asked for counts for the whole frame
    const QRectF pic = Tracker::pictureRect(format, c);
    const int frameEdge = std::max(format.width, format.height);
    const double picW = std::max(1.0, pic.width()), picLongest = std::max({1.0, pic.width(), pic.height()});
    const int edge = st.mode == TrackMode::Face
                         ? std::clamp(int(std::ceil(kFaceEdge * frameEdge / picLongest)), kFaceEdge, std::max(kFaceEdge, frameEdge))
                         : std::clamp(int(std::ceil(kTemplateMax / st.size * frameEdge / picW)), 320, frameEdge);
    const auto source = Tracker::clipPictures(format, c);
    return trackFrames([&](int f) { return source(f, edge); }, st, from, to, progress, cancel, faces);
}

} // namespace Picture::Comp
