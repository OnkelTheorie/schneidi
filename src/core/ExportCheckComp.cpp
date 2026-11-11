// SPDX-License-Identifier: AGPL-3.0-only
// Checks of compositing graphs (0.19, effect "comp"): nodes that do not reach the output, graphs too slow to play,
// soft key mattes. Pure on the timeline (key_edges on a matte the caller rendered).
#include "core/ExportCheck.h"

#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"

#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace ExportCheck {

namespace {

QString num(double v, int decimals = 0) { return QString::number(v, 'f', decimals).replace('.', I18n::decimalPoint()); }

// ms per frame at 1920x1080 on one core (rough; blur and shadow are the slow ones)
double kindCostMs(const QString& type)
{
    static const QHash<QString, double> costs{
        {"clip", 0},   {"media", 6},  {"color", 1},  {"gradient", 3},   {"merge", 4},   {"transform", 8},
        {"crop", 2},   {"blur", 70},  {"grade", 6},  {"fx", 10},        {"mask", 3},    {"matte", 1},
        {"key", 8},    {"matte_tune", 8}, {"tracker", 1}, {"text", 5}, {"shadow", 18}, {"glitch", 8}};
    return costs.value(type, 5);
}

} // namespace

double compCostMs(const EffectInstance& comp, QSize size)
{
    CompGraph g;
    if (!Comp::graphOf(comp, &g) || size.isEmpty()) return 0;
    double ms = 0;
    for (int id : g.evalOrder())
        if (const CompNode* n = g.node(id)) {
            double c = kindCostMs(n->type);
            if (n->type == QLatin1String("blur")) // a small radius is cheaper (the default 8 is the 70 ms measured)
                c *= std::clamp(comp.params.value(CompGraph::paramKey(id, "radius"), 8.0).toDouble() / 8.0, 0.25, 4.0);
            ms += c;
        }
    return ms * double(size.width()) * size.height() / (1920.0 * 1080.0);
}

QVector<Issue> comp(const Timeline& tl, int from, int to, const ProjectFormat& f)
{
    QVector<Issue> out;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled || c.end() <= from || c.start >= to) continue;
            const EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
            CompGraph g;
            if (!e || !e->enabled || !Comp::graphOf(*e, &g) || g.isEmpty()) continue;
            const int a = std::max(from, c.start), b = std::min(to, c.end());
            if (const CompNode* o = g.node(g.out); o && Comp::outputsMask(o->type))
                out << Issue{Severity::Warning, QStringLiteral("comp_output_mask"),
                             T("Compositing-Graph: der Ausgabe-Knoten %1 (%2) gibt eine Maske aus, kein Bild - der Graph "
                               "wirkt nicht. Ein Bild-Knoten als Ausgabe (Maske in den Masken-Eingang eines Merge)")
                                 .arg(o->id).arg(o->type),
                             a, b, c.id};
            const QVector<int> order = g.evalOrder();
            // A tracker is read through `follow` (a value, not a link): followed nodes count as used
            QSet<int> followed;
            for (int id : order)
                if (const int f = Comp::followId(e->params.value(CompGraph::paramKey(id, QStringLiteral("follow"))).toDouble()))
                    followed.insert(f);
            QStringList unused;
            for (const CompNode& n : g.nodes)
                if (!order.contains(n.id) && !followed.contains(n.id))
                    unused << QStringLiteral("%1 (%2)").arg(n.id).arg(n.type);
            if (!unused.isEmpty())
                out << Issue{Severity::Info, QStringLiteral("comp_unused_input"),
                             T("Compositing-Graph: Knoten %1 erreichen die Ausgabe nicht und wirken nicht").arg(unused.join(", ")),
                             a, b, c.id};
            const double ms = compCostMs(*e, f.size());
            if (ms > kCompBudgetMs && !c.renderCache)
                out << Issue{Severity::Info, QStringLiteral("comp_slow"),
                             T("Compositing-Graph rechnet etwa %1 ms je Bild (mehr als %2 ms): Render-Cache für diesen Clip "
                               "einschalten, damit die Wiedergabe flüssig bleibt")
                                 .arg(num(ms), num(kCompBudgetMs)),
                             a, b, c.id};
        }
    }
    return out;
}

std::optional<Issue> keyEdges(const uint8_t* alpha, int w, int h, int clip, int from, int to)
{
    if (!alpha || w <= 0 || h <= 0) return std::nullopt;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (alpha[size_t(y) * w + x] >= 240) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < x0) return std::nullopt; // nothing solid: no subject to judge
    int half = 0, all = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const uint8_t v = alpha[size_t(y) * w + x];
            half += v >= 16 && v < 240;
            ++all;
        }
    const double share = double(half) / std::max(1, all);
    if (share <= 0.08) return std::nullopt;
    return Issue{Severity::Info, QStringLiteral("key_edges"),
                 T("Key unsauber: %1 % halbdurchsichtige Pixel im Motiv (Key-Werte nachstellen oder matte_tune)")
                     .arg(num(share * 100)),
                 from, to, clip};
}

} // namespace ExportCheck
