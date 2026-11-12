// SPDX-License-Identifier: AGPL-3.0-only
// Checks of compositing graphs (0.19, effect "comp"): nodes that do not reach the output, graphs too slow to play,
// soft key mattes, trackers that lost their point (0.22). Pure on the timeline (key_edges on a matte the caller rendered).
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
        {"key", 8},    {"matte_tune", 8}, {"tracker", 1}, {"text", 5}, {"shadow", 18}, {"glitch", 8}, {"camera", 0}};
    return costs.value(type, 5);
}

// Timeline frame as a clock time "MM:SS" ("H:MM:SS" from an hour)
QString clock(int frame, const ProjectFormat& f)
{
    const double fps = f.rate.fps() > 0 ? f.rate.fps() : 25;
    const int s = int(std::floor(std::max(0, frame) / fps));
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
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
            // A tracker is read through `follow`, a camera through `camera` (values, not links): they count as used
            const QSet<int> followed = Comp::valueReads(*e, g, order);
            QStringList unused;
            for (const CompNode& n : g.nodes)
                if (!order.contains(n.id) && !followed.contains(n.id))
                    unused << QStringLiteral("%1 (%2)").arg(n.id).arg(n.type);
            if (!unused.isEmpty())
                out << Issue{Severity::Info, QStringLiteral("comp_unused_input"),
                             T("Compositing-Graph: Knoten %1 erreichen die Ausgabe nicht und wirken nicht").arg(unused.join(", ")),
                             a, b, c.id};
            // A depth without a camera (0.22): z set, but `camera` names no camera node - the layer does not move
            QStringList noCamera;
            for (int id : order)
                if (const CompNode* n = g.node(id); n && Comp::takesCamera(n->type)
                    && e->params.value(CompGraph::paramKey(id, QString::fromLatin1(Comp::kLayerDepthKey))).toDouble() > 0
                    && Comp::cameraOf(*e, g, id) == 0)
                    noCamera << QString::number(id);
            if (!noCamera.isEmpty())
                out << Issue{Severity::Info, QStringLiteral("comp_camera_missing"),
                             T("Compositing-Graph: Knoten %1 haben eine Tiefe im Raum, aber keine Kamera - die Tiefe "
                               "wirkt nicht (einen Kamera-Knoten hinzufügen; Wert „Kamera“ = seine Id oder 0)").arg(noCamera.join(", ")),
                             a, b, c.id};
            // Trackers others follow that lost their point (keys n<id>.seen): each gap followers do not bridge, and
            // a track that ends lost (gave up, or the analysis ended there)
            for (int id : std::as_const(followed)) {
                const CompNode* n = g.node(id);
                if (!n || n->type != QLatin1String(Comp::kTrackerType)) continue;
                for (const Comp::TrackGap& gap : Comp::trackGaps(c, id)) {
                    if (gap.bridged()) continue;
                    const int ga = std::max(a, c.start + gap.from), gb = std::min(b, c.start + gap.to + 1);
                    if (gb <= ga) continue;
                    if (gap.openEnd || gap.openStart)
                        out << Issue{Severity::Warning, QStringLiteral("tracker_ended"),
                                     gap.openStart
                                         ? T("Tracker %1 findet den Punkt erst ab %2: davor folgt ihm nichts sicher "
                                             "(neu tracken, wo der Punkt sichtbar ist)")
                                               .arg(id).arg(clock(c.start + gap.to + 1, f))
                                         : T("Tracker %1 verliert den Punkt ab %2 und findet ihn nicht wieder: die Spur "
                                             "endet dort (ab einem Bild neu tracken, wo er wieder sichtbar ist)")
                                               .arg(id).arg(clock(c.start + gap.from, f)),
                                     ga, gb, c.id};
                    else
                        out << Issue{Severity::Warning, QStringLiteral("tracker_lost"),
                                     T("Tracker %1 verliert den Punkt bei %2–%3 (%4 Bilder)")
                                         .arg(id).arg(clock(c.start + gap.from, f), clock(c.start + gap.to, f))
                                         .arg(gap.length()),
                                     ga, gb, c.id};
                }
            }
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
