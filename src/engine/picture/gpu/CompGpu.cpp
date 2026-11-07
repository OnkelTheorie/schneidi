// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/gpu/CompGpu.h"

#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/Masks.h"
#include "core/Types.h"
#include "engine/ColorGrade.h"
#include "engine/VideoEffects.h"
#include "engine/video/Blend.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace Picture::Gpu {

struct CompPlan {
    struct Step {
        enum Op { Upload, Color, Merge, Mask, Key, Blur, Draw };
        Op op = Upload;
        bool mask = false;       // the step's picture is a mask (value in every channel)
        int in[3] = {-1, -1, -1}; // steps read as t0..t2 (-1 = none)
        float p[40] = {};
        // Upload: straight RGBA (mask: the value in every channel, premultiplied = as stored)
        std::shared_ptr<std::vector<uint8_t>> pixels;
        // Key: its table (chroma 257 x 257 RGBA8, luma 256 x 1 gray)
        std::shared_ptr<std::vector<uint8_t>> table;
        bool luma = false;
        // Blur: box radii (rows, columns), then a mix by mask when in[1] >= 0
        int radii[3] = {0, 0, 0};
        // Draw: the affine map (transform), the source rectangle kept (crop), the grade (LUT on the card)
        Compose::Affine m;
        QRectF crop;
        bool graded = false;
        ColorGrade::Params grade;
        int mixMask = -1; // Draw: then mixed with its source by this mask step
    };
    QSize size;
    std::vector<Step> steps;
    int out = -1;
};

QSize planSize(const CompPlan& plan) { return plan.size; }

namespace {

const QStringList& mergeModes() // CompNodesCore's choice list (index stored)
{
    static const QStringList m = {"normal", "add", "screen", "multiply", "overlay", "darken", "lighten", "difference"};
    return m;
}

constexpr int kMaxBoxRadius = 64; // larger blurs stay on the CPU (one shader tap per box pixel)

bool fail(QString* why, const QString& s)
{
    if (why) *why = s;
    return false;
}

QRect pixelRect(const QRectF& r, int w, int h) // MaskRaster's rows drawn
{
    if (r.isEmpty()) return {};
    const double x0 = std::max(-1.0, std::floor(r.left())), y0 = std::max(-1.0, std::floor(r.top()));
    const double x1 = std::min(double(w), std::ceil(r.right())), y1 = std::min(double(h), std::ceil(r.bottom()));
    if (x1 <= x0 || y1 <= y0) return {};
    return QRect(QPoint(int(x0), int(y0)), QPoint(int(x1), int(y1))).intersected(QRect(0, 0, w, h));
}

void setRect(float* p, const QRect& r)
{
    if (r.isEmpty()) {
        p[0] = p[1] = 0;
        p[2] = p[3] = -1;
        return;
    }
    p[0] = float(r.left()), p[1] = float(r.top()), p[2] = float(r.right()), p[3] = float(r.bottom());
}

// GPU thread: baked grades as 3D LUTs (the last few; keyframed grades bake one per frame)
struct CompCaches {
    struct Lut {
        ColorGrade::Params params;
        LutPtr lut;
    };
    std::vector<Lut> luts;
    LutPtr lut(Context& g, const ColorGrade::Params& p)
    {
        for (size_t i = 0; i < luts.size(); ++i)
            if (luts[i].params == p) {
                std::rotate(luts.begin(), luts.begin() + i, luts.begin() + i + 1);
                return luts.front().lut;
            }
        LutPtr l = g.gradeLut(*ColorGrade::bake(p, nullptr));
        if (!l) return {};
        luts.insert(luts.begin(), {p, l});
        if (luts.size() > 8) luts.pop_back();
        return l;
    }
};

} // namespace

CompPlanPtr planComp(const EffectInstance& comp, const Comp::Env& env, Comp::Cache* cache, QString* why)
{
    using Step = CompPlan::Step;
    if (env.size.isEmpty()) return fail(why, "no size"), nullptr;
    CompGraph g;
    if (!::Comp::graphOf(comp, &g)) return fail(why, "no graph"), nullptr;
    const QVector<int> order = g.evalOrder();
    if (order.isEmpty() || !g.node(g.out)) return fail(why, "empty graph"), nullptr;
    const int W = env.size.width(), H = env.size.height();

    // Values per node and which have keyframes (as CompEval)
    QHash<int, QVariantMap> values;
    for (auto it = comp.params.cbegin(); it != comp.params.cend(); ++it) {
        int id = 0;
        QString key;
        if (CompGraph::splitParamKey(it.key(), &id, &key)) values[id].insert(key, it.value());
    }
    QHash<int, QSet<QString>> animated;
    if (env.clip)
        for (auto it = env.clip->keys.cbegin(); it != env.clip->keys.cend(); ++it) {
            QString effect, full, key;
            int id = 0;
            if (!it.value().isEmpty() && Keys::effectParamOf(it.key(), &effect, &full)
                && effect == QLatin1String(::Comp::kEffectId) && CompGraph::splitParamKey(full, &id, &key))
                animated[id].insert(key);
        }
    auto num = [&](int id, const Comp::NodeType& k, const QString& key) -> double {
        if (env.clip && animated.value(id).contains(key))
            return Keys::valueAt(*env.clip, Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(id, key)), env.t);
        return Comp::numValue(k, values.value(id), key);
    };
    auto color = [&](int id, const Comp::NodeType& k, const QString& key) {
        if (env.clip && animated.value(id).contains(key)) return Keys::toColor(num(id, k, key));
        return Comp::colorValue(k, values.value(id), key);
    };
    auto text = [&](int id, const Comp::NodeType& k, const QString& key) { return Comp::textValue(k, values.value(id), key); };

    auto plan = std::make_shared<CompPlan>();
    plan->size = env.size;
    QHash<int, int> stepOf;    // node -> its step (-1 = made nothing)
    QSet<int> onCard;          // nodes made by a card step
    QHash<int, std::shared_ptr<std::vector<uint8_t>>> keyTables; // chroma key tables made while deciding
    auto inputStep = [&](int id, const char* name) -> int {
        const CompLink* l = g.linkInto(id, QLatin1String(name));
        return l ? stepOf.value(l->from, -1) : -1;
    };
    auto isMask = [&](int step) { return step >= 0 && plan->steps[size_t(step)].mask; };
    auto rgbaIn = [&](int id, const char* name) {
        const int s = inputStep(id, name);
        return isMask(s) ? -1 : s;
    };
    auto push = [&](Step&& s) {
        plan->steps.push_back(std::move(s));
        return int(plan->steps.size()) - 1;
    };

    // Which nodes the card does (decided from the values at t)
    auto cardKind = [&](int id, const CompNode& n, const Comp::NodeType* k, QString* reason) -> bool {
        if (!k) return fail(reason, "unknown kind " + n.type);
        const QString& t = k->id;
        if (t == QLatin1String("merge") || t == QLatin1String("color")) return true;
        if (t == QLatin1String("mask")) {
            Masks::Shape shape;
            if (!Masks::shapeFromId(text(id, *k, "shape"), &shape)) shape = Masks::Shape::Rect;
            if (shape == Masks::Shape::Polygon || shape == Masks::Shape::Bezier) return fail(reason, "polygon mask");
            if (num(id, *k, "follow") > 0) return fail(reason, "mask follows a tracker");
            return true;
        }
        if (t == QLatin1String("key")) {
            if (num(id, *k, "mode") >= 0.5) return true;
            VideoEffects::KeyParams p;
            p.color = color(id, *k, "color");
            p.tolerance = num(id, *k, "tolerance");
            p.softness = num(id, *k, "softness");
            p.spill = num(id, *k, "spill");
            auto table = std::make_shared<std::vector<uint8_t>>(size_t(VideoEffects::keyTableSide) * VideoEffects::keyTableSide * 4);
            if (!VideoEffects::keyTable(p, table->data())) return fail(reason, "gray key");
            keyTables.insert(id, std::move(table));
            return true;
        }
        if (t == QLatin1String("blur")) {
            int radii[3];
            const double sigma = std::max(0.0, num(id, *k, "radius")) * env.scale;
            if (VideoEffects::matteBlurRadii(sigma, radii) && *std::max_element(radii, radii + 3) > kMaxBoxRadius)
                return fail(reason, "blur too large");
            return true;
        }
        if (t == QLatin1String("transform")) {
            if (num(id, *k, "follow") > 0) return fail(reason, "transform follows a tracker");
            if (std::abs(num(id, *k, "size")) < 0.5) return fail(reason, "transform shrinks below 1/2");
            return true;
        }
        if (t == QLatin1String("crop")) return std::max(0.0, num(id, *k, "soft")) * env.scale < 0.5 || fail(reason, "soft crop");
        if (t == QLatin1String("grade")) return text(id, *k, "lut").isEmpty() || fail(reason, "grade with a LUT file");
        return fail(reason, "node " + t);
    };

    for (int id : order) {
        const CompNode* n = g.node(id);
        if (!n) continue;
        const Comp::NodeType* k = Comp::find(n->type);
        QString reason;
        const bool card = cardKind(id, *n, k, &reason);
        bool fedByCard = false;
        for (const CompLink& l : g.links)
            if (l.to == id && onCard.contains(l.from)) fedByCard = true;
        if (!card) {
            if (fedByCard) {
                if (why) *why = reason + " after a node on the card";
                return nullptr;
            }
            continue; // made on the CPU when a card node (or the output) reads it
        }
        // The CPU nodes this one reads: evaluated now (their whole subgraph, CompEval with its cache) and uploaded
        for (const CompLink& l : g.links) {
            if (l.to != id || onCard.contains(l.from) || stepOf.contains(l.from)) continue;
            const CompNode* from = g.node(l.from);
            const Comp::NodeType* fk = from ? Comp::find(from->type) : nullptr;
            Step up;
            up.op = Step::Upload;
            if (fk && fk->output == Comp::Buf::Mask) {
                // A mask the card cannot draw (polygon, bezier, matte): its node alone, a leaf only
                bool leaf = true;
                for (const CompLink& l2 : g.links) leaf = leaf && l2.to != l.from;
                if (!leaf || !fk->eval) return fail(why, "mask node " + from->type + " with inputs"), nullptr;
                Comp::NodeCtx ctx;
                ctx.node = l.from;
                ctx.t = env.t;
                ctx.frameNo = env.frameNo;
                ctx.size = env.size;
                ctx.scale = env.scale;
                ctx.fps = env.fps;
                ctx.cancel = env.cancel;
                const int fid = l.from;
                ctx.num = [&, fid, fk](const QString& key) { return num(fid, *fk, key); };
                ctx.color = [&, fid, fk](const QString& key) { return color(fid, *fk, key); };
                ctx.text = [&, fid, fk](const QString& key) { return text(fid, *fk, key); };
                ctx.numOf = [&](int other, const QString& key) {
                    const CompNode* o = g.node(other);
                    const Comp::NodeType* ok = o ? Comp::find(o->type) : nullptr;
                    if (!ok) return 0.0;
                    if (o->type == QLatin1String(::Comp::kTrackerType)
                        && (key == QLatin1String("x") || key == QLatin1String("y"))
                        && !values.value(other).contains(key) && !animated.value(other).contains(key))
                        return num(other, *ok, key == QLatin1String("x") ? QStringLiteral("ax") : QStringLiteral("ay"));
                    return num(other, *ok, key);
                };
                ctx.input = [](const QString&) -> const Comp::Buf* { return nullptr; };
                ctx.take = [&](Comp::Buf::Kind kind) { return Comp::Buf(kind, W, H); };
                Comp::Buf b;
                if (!fk->eval(ctx, &b) || b.kind != Comp::Buf::Mask || b.w != W || b.h != H) {
                    stepOf[l.from] = -1;
                    continue;
                }
                auto px = std::make_shared<std::vector<uint8_t>>(size_t(W) * H * 4);
                uint8_t* o = px->data();
                for (size_t i = 0, e = size_t(W) * H; i < e; ++i, o += 4) o[0] = o[1] = o[2] = o[3] = b.px[i];
                up.mask = true;
                up.pixels = std::move(px);
            } else {
                EffectInstance sub = comp;
                CompGraph part = g;
                part.out = l.from;
                ::Comp::setGraph(sub, part);
                Comp::Buf b;
                if (!Comp::evaluate(sub, env, cache, &b)) {
                    stepOf[l.from] = -1;
                    continue;
                }
                up.pixels = std::make_shared<std::vector<uint8_t>>(std::move(b.px));
            }
            stepOf[l.from] = push(std::move(up));
        }

        Step s;
        const QString& t = k->id;
        if (t == QLatin1String("color")) {
            const QColor c = color(id, *k, "color");
            const float a = float(c.alpha()) / 255.0f; // the bytes the CPU fills in, premultiplied
            s.op = Step::Color;
            s.p[0] = c.red() / 255.0f * a, s.p[1] = c.green() / 255.0f * a, s.p[2] = c.blue() / 255.0f * a, s.p[3] = a;
        } else if (t == QLatin1String("merge")) {
            const int bg = rgbaIn(id, "bg"), fg = rgbaIn(id, "fg"), mask = inputStep(id, "mask");
            const double opacity = std::clamp(num(id, *k, "opacity"), 0.0, 1.0);
            if (bg < 0 && fg < 0) {
                stepOf[id] = -1;
                onCard.insert(id);
                continue;
            }
            if (fg < 0 || opacity <= 0) { // the background as it is
                stepOf[id] = bg;
                onCard.insert(id);
                continue;
            }
            s.op = Step::Merge;
            s.in[0] = bg, s.in[1] = fg, s.in[2] = mask;
            s.p[0] = float(Blend::modeIndex(mergeModes().value(int(num(id, *k, "mode")))));
            s.p[1] = float(std::lround(opacity * 256));
            s.p[2] = bg >= 0 ? 1 : 0;
            s.p[3] = 1;
            s.p[4] = mask >= 0 ? 1 : 0;
        } else if (t == QLatin1String("mask")) {
            Masks::Spec sp;
            Masks::shapeFromId(text(id, *k, "shape"), &sp.shape);
            sp.center = QPointF(num(id, *k, "x"), num(id, *k, "y"));
            sp.width = num(id, *k, "w");
            sp.height = num(id, *k, "h");
            sp.rotation = num(id, *k, "rotate");
            sp.soft = num(id, *k, "soft") * env.scale;
            sp.invert = num(id, *k, "invert") != 0;
            const bool gradient = sp.shape == Masks::Shape::Linear || sp.shape == Masks::Shape::Radial;
            if (gradient)
                for (int i = 0; i < 2; ++i)
                    sp.points.append(QPointF(num(id, *k, QStringLiteral("p%1x").arg(i)), num(id, *k, QStringLiteral("p%1y").arg(i))));
            const Masks::Geometry geo = Masks::toPixels(sp, env.size);
            s.op = Step::Mask;
            s.mask = true;
            s.p[0] = float(sp.shape == Masks::Shape::Rect ? 0 : sp.shape == Masks::Shape::Ellipse ? 1
                           : sp.shape == Masks::Shape::Linear ? 2 : 3);
            s.p[1] = sp.invert ? 1 : 0;
            s.p[2] = -1;
            const int in = inputStep(id, "mask");
            if (isMask(in)) { // combined with the mask at its input (CompNodesMask takes only a mask there)
                Masks::Combine mode = Masks::Combine::Add;
                Masks::combineFromId(text(id, *k, "combine"), &mode);
                s.p[2] = mode == Masks::Combine::Add ? 0 : mode == Masks::Combine::Subtract ? 1 : 2;
                s.in[0] = in;
            }
            QRect drawn;
            if (!geo.empty) {
                if (gradient) {
                    drawn = pixelRect(Masks::bounds(geo), W, H);
                    s.p[16] = float(geo.a.x()), s.p[17] = float(geo.a.y()), s.p[18] = float(geo.b.x()), s.p[19] = float(geo.b.y());
                } else {
                    const GradeWindows::Geometry wg = Masks::toWindow(geo, env.size);
                    drawn = GradeWindows::Mask(wg, W, H).bounds();
                    const double rad = wg.rotation * std::numbers::pi / 180.0;
                    s.p[4] = float(wg.x / 100.0 * W), s.p[5] = float(wg.y / 100.0 * H);
                    s.p[6] = float(std::max(0.5, wg.width / 100.0 * W / 2)), s.p[7] = float(std::max(0.5, wg.height / 100.0 * H / 2));
                    s.p[8] = float(std::cos(rad)), s.p[9] = float(std::sin(rad));
                    s.p[10] = float(std::max(1.0, wg.softness / 100.0 * H));
                }
            }
            setRect(s.p + 12, drawn);
        } else if (t == QLatin1String("key")) {
            const int src = rgbaIn(id, "src");
            if (src < 0) {
                stepOf[id] = -1;
                onCard.insert(id);
                continue;
            }
            s.op = Step::Key;
            s.in[0] = src;
            if (num(id, *k, "mode") >= 0.5) {
                VideoEffects::LumaKeyParams p;
                p.low = num(id, *k, "low");
                p.high = num(id, *k, "high");
                p.soft = num(id, *k, "soft");
                p.invert = num(id, *k, "invert") > 0.5;
                s.luma = true;
                s.p[0] = 1;
                s.table = std::make_shared<std::vector<uint8_t>>(256);
                VideoEffects::lumaKeyTable(p, s.table->data());
            } else {
                s.table = keyTables.value(id);
                if (!s.table) return fail(why, "gray key"), nullptr;
            }
        } else if (t == QLatin1String("blur")) {
            const int src = rgbaIn(id, "src");
            if (src < 0) {
                stepOf[id] = -1;
                onCard.insert(id);
                continue;
            }
            const double sigma = std::max(0.0, num(id, *k, "radius")) * env.scale;
            if (!VideoEffects::matteBlurRadii(sigma, s.radii)) { // no blur: the source as it is
                stepOf[id] = src;
                onCard.insert(id);
                continue;
            }
            s.op = Step::Blur;
            s.in[0] = src;
            s.in[1] = inputStep(id, "mask");
        } else if (t == QLatin1String("transform") || t == QLatin1String("crop") || t == QLatin1String("grade")) {
            const int src = rgbaIn(id, "src");
            if (src < 0) {
                stepOf[id] = -1;
                onCard.insert(id);
                continue;
            }
            s.op = Step::Draw;
            s.in[0] = src;
            bool identity = true;
            if (t == QLatin1String("transform")) { // CompNodesCore transformOf without a tracker
                const double x = num(id, *k, "x"), y = num(id, *k, "y"), size = num(id, *k, "size");
                const double angle = num(id, *k, "angle");
                const double ax = num(id, *k, "ax") * W, ay = num(id, *k, "ay") * H;
                const double rad = angle * std::numbers::pi / 180.0, cs = std::cos(rad) * size, sn = std::sin(rad) * size;
                Compose::Affine& m = s.m;
                m.a = cs;
                m.b = -sn;
                m.d = sn;
                m.e = cs;
                m.c = x * W - (cs * ax - sn * ay);
                m.f = y * H - (sn * ax + cs * ay);
                identity = std::abs(m.a - 1) < 1e-9 && std::abs(m.e - 1) < 1e-9 && std::abs(m.b) < 1e-9
                           && std::abs(m.d) < 1e-9 && std::abs(m.c) < 1e-9 && std::abs(m.f) < 1e-9;
                if (identity) s.m = {};
            } else if (t == QLatin1String("crop")) {
                const double x0 = std::clamp(num(id, *k, "left"), 0.0, 1.0) * W, y0 = std::clamp(num(id, *k, "top"), 0.0, 1.0) * H;
                const double x1 = (1 - std::clamp(num(id, *k, "right"), 0.0, 1.0)) * W;
                const double y1 = (1 - std::clamp(num(id, *k, "bottom"), 0.0, 1.0)) * H;
                if (x0 > 0 || y0 > 0 || x1 < W || y1 < H) {
                    identity = false;
                    // An empty rectangle crops everything (Layer::crop empty = no crop)
                    s.crop = x1 > x0 && y1 > y0 ? QRectF(QPointF(x0, y0), QPointF(x1, y1)) : QRectF(0, 0, 1e-6, 1e-6);
                }
            } else {
                ColorGrade::Params p;
                static constexpr const char* wheels[] = {"lift", "gamma", "gain", "offset"};
                static constexpr const char* parts[] = {"y", "r", "g", "b"};
                for (int i = 0; i < 4; ++i) {
                    double* wheel = i == 0 ? p.lift : i == 1 ? p.gamma : i == 2 ? p.gain : p.offset;
                    for (int j = 0; j < 4; ++j)
                        wheel[j] = num(id, *k, QStringLiteral("%1_%2").arg(QLatin1String(wheels[i]), QLatin1String(parts[j])));
                }
                p.contrast = num(id, *k, "contrast");
                p.pivot = num(id, *k, "pivot");
                p.saturation = num(id, *k, "saturation");
                p.temperature = num(id, *k, "temperature");
                p.tint = num(id, *k, "tint");
                p.exposure = num(id, *k, "exposure");
                if (!p.isNeutral()) {
                    identity = false;
                    s.graded = true;
                    s.grade = p;
                    s.mixMask = inputStep(id, "mask");
                }
            }
            if (identity) {
                stepOf[id] = src;
                onCard.insert(id);
                continue;
            }
        }
        stepOf[id] = push(std::move(s));
        onCard.insert(id);
    }
    if (onCard.isEmpty()) return fail(why, "no node for the card"), nullptr;
    if (!onCard.contains(g.out)) return fail(why, "output on the CPU"), nullptr;
    plan->out = stepOf.value(g.out, -1);
    if (plan->out < 0 || isMask(plan->out)) return fail(why, "output makes no picture"), nullptr;
    return plan;
}

TexturePtr runComp(Context& g, const CompPlan& plan, std::vector<TexturePtr>* keep)
{
    using Step = CompPlan::Step;
    if (!g.compSupported() || plan.out < 0) return {};
    auto& slot = g.compData();
    if (!slot) slot = std::make_shared<CompCaches>();
    CompCaches& caches = *static_cast<CompCaches*>(slot.get());
    const QSize size = plan.size;
    std::vector<TexturePtr> t(plan.steps.size());
    auto tex = [&](int i) -> const Texture* { return i >= 0 && size_t(i) < t.size() ? t[size_t(i)].get() : nullptr; };
    for (size_t i = 0; i < plan.steps.size(); ++i) {
        const Step& s = plan.steps[i];
        TexturePtr out;
        switch (s.op) {
        case Step::Upload: {
            if (!s.pixels || s.pixels->size() != size_t(size.width()) * size.height() * 4) break;
            TexturePtr up = g.uploadRgba(s.pixels->data(), size.width(), size.height(), s.mask);
            out = g.compTarget(size);
            if (!up || !out) break;
            g.draw(*out, {up.get(), {}, 1.0, nullptr, {}});
            keep->push_back(std::move(up));
            break;
        }
        case Step::Color: out = g.compTarget(size, s.p); break;
        case Step::Merge: out = g.compPass(Context::CompShader::Merge, size, s.p, tex(s.in[0]), tex(s.in[1]), tex(s.in[2])); break;
        case Step::Mask: out = g.compPass(Context::CompShader::Mask, size, s.p, tex(s.in[0])); break;
        case Step::Key: {
            if (!s.table) break;
            TexturePtr table = s.luma ? g.uploadGray(s.table->data(), 256, 1)
                                      : g.uploadRgba(s.table->data(), VideoEffects::keyTableSide, VideoEffects::keyTableSide, false);
            if (!table) break;
            out = g.compPass(Context::CompShader::Key, size, s.p, tex(s.in[0]), table.get());
            keep->push_back(std::move(table));
            break;
        }
        case Step::Blur: {
            const Texture* src = tex(s.in[0]);
            if (!src) break;
            TexturePtr cur;
            for (int r : s.radii) {
                for (int axis = 0; axis < 2; ++axis) {
                    float p[40] = {};
                    p[0] = axis == 0 ? 1 : 0;
                    p[1] = axis == 0 ? 0 : 1;
                    p[2] = float(std::min(r, axis == 0 ? size.width() : size.height()));
                    TexturePtr next = g.compPass(Context::CompShader::Blur, size, p, cur ? cur.get() : src);
                    if (cur) keep->push_back(std::move(cur));
                    cur = std::move(next);
                    if (!cur) break;
                }
                if (!cur) break;
            }
            out = std::move(cur);
            if (out && s.in[1] >= 0 && tex(s.in[1])) { // only where the mask is
                float p[40] = {};
                p[5] = 1; // kind: mix by mask
                TexturePtr mixed = g.compPass(Context::CompShader::Merge, size, p, src, out.get(), tex(s.in[1]));
                keep->push_back(std::move(out));
                out = std::move(mixed);
            }
            break;
        }
        case Step::Draw: {
            const Texture* src = tex(s.in[0]);
            if (!src) break;
            LutPtr lut = s.graded ? caches.lut(g, s.grade) : nullptr;
            if (s.graded && !lut) break;
            out = g.compTarget(size);
            if (!out) break;
            g.draw(*out, {src, s.m, 1.0, lut.get(), s.crop});
            if (s.mixMask >= 0 && tex(s.mixMask)) {
                float p[40] = {};
                p[5] = 1;
                TexturePtr mixed = g.compPass(Context::CompShader::Merge, size, p, src, out.get(), tex(s.mixMask));
                keep->push_back(std::move(out));
                out = std::move(mixed);
            }
            break;
        }
        }
        if (!out) { // the card refused a step: nothing of this graph
            for (TexturePtr& x : t)
                if (x) keep->push_back(std::move(x));
            return {};
        }
        t[i] = std::move(out);
    }
    TexturePtr result = t[size_t(plan.out)];
    for (TexturePtr& x : t)
        if (x) keep->push_back(std::move(x));
    return result;
}

} // namespace Picture::Gpu
