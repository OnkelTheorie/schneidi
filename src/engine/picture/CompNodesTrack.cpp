// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package D (0.19): tracker
//
// tracker (input src): passes its picture through unchanged, so it can sit anywhere in a chain. What it tracks is
// measured once by the analysis (engine/picture/CompTrack, Editor::trackCompNode) and stored as keyframes of its own
// result parameters x, y, scale, angle; other nodes read them with numOf(id, "x") ("follow" = this node's id).
// Parameters: mode point|two|face; start points ax/ay (point, face: where the face is), bx/by (second point) 0..1;
// size = side of the square looked for, relative to the picture width; results x/y 0..1, scale (1 = as at the start),
// angle (degrees clockwise against the start); rx/ry = the reference point followers are drawn relative to (the
// follow rule: core/CompTrack.h). Losing the point (0.22): seen = keyframes 1 found / 0 lost (written by the analysis),
// lost = what followers do in a gap (bridge|hold|fade), max_lost = frames in a row lost before the analysis stops.
// Four corners (0.22): mode quad, start corners q0x … q3y (clockwise from the top left), results c0x … c3y as keyframes
// plus x/y/scale/angle of the quad's middle (core/CompTrack.h).
//
// corner_pin (input src): lays its picture onto four corners in perspective (a screen, a sign, a wall): the corners
// c0x … c3y (0..1, clockwise from the top left) by hand, or those of a quad tracker it follows (follow = its id; a
// point/two/face tracker moves, scales and turns the own corners by the follow rule). The picture's content area
// (a media fitted with bars: the picture itself) is stretched onto them; transparent outside.
#include "core/CompTrack.h"
#include "core/I18n.h"
#include "engine/picture/CompNodes.h"

#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Picture::Comp {

namespace {

EffectParam number(const char* key, const QString& label, double def, double lo, double hi, double step, int decimals,
                   const QString& description = {})
{
    EffectParam p;
    p.key = QString::fromLatin1(key);
    p.label = label;
    p.type = EffectParam::Double;
    p.defaultValue = def;
    p.min = lo;
    p.max = hi;
    p.step = step;
    p.decimals = decimals;
    p.description = description;
    return p;
}

bool evalCornerPin(NodeCtx& ctx, Buf* out);

bool passThrough(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input(QStringLiteral("src"));
    if (!src) return false;
    Buf b = ctx.take(src->kind);
    if (b.w == src->w && b.h == src->h && b.px.size() == src->px.size()) std::copy(src->px.begin(), src->px.end(), b.px.begin());
    else b = *src; // another size than the output (only from a test): as it is
    b.area = src->area;
    *out = std::move(b);
    return true;
}

} // namespace

bool cornerPinCorners(const std::function<double(const QString&)>& num,
                      const std::function<double(int, const QString&)>& numOf, QSize size, QPointF out[4], double* seen)
{
    const double W = size.width(), H = size.height();
    *seen = 1;
    QPointF c[4];
    for (int i = 0; i < 4; ++i)
        c[i] = QPointF(num(QString::fromLatin1(::Comp::kCornerKeys[2 * i])), num(QString::fromLatin1(::Comp::kCornerKeys[2 * i + 1])));
    const int follow = ::Comp::followId(num(QStringLiteral("follow")));
    if (follow > 0 && numOf) {
        const double v = numOf(follow, QString::fromLatin1(::Comp::kSeenKey));
        *seen = std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 1.0;
        if (numOf(follow, QString::fromLatin1(::Comp::kQuadKey)) > 0.5) { // the tracker's corners
            for (int i = 0; i < 4; ++i)
                c[i] = QPointF(numOf(follow, QString::fromLatin1(::Comp::kCornerKeys[2 * i])),
                               numOf(follow, QString::fromLatin1(::Comp::kCornerKeys[2 * i + 1])));
        } else { // the follow rule (core/CompTrack.h): moved by (x - rx, y - ry), scaled and turned around (x, y)
            double ts = numOf(follow, QStringLiteral("scale"));
            ts = ts > 0 && std::isfinite(ts) ? ts : 1.0;
            double ta = numOf(follow, QStringLiteral("angle"));
            ta = std::isfinite(ta) ? ta : 0.0;
            const double tr = ta * std::numbers::pi / 180.0, tc = std::cos(tr), tsn = std::sin(tr);
            const double rx = numOf(follow, QStringLiteral("rx")), ry = numOf(follow, QStringLiteral("ry"));
            const double x = numOf(follow, QStringLiteral("x")), y = numOf(follow, QStringLiteral("y"));
            for (QPointF& p : c) {
                const double dx = (p.x() - rx) * W, dy = (p.y() - ry) * H;
                p = QPointF(x + ts * (tc * dx - tsn * dy) / W, y + ts * (tsn * dx + tc * dy) / H);
            }
        }
    }
    *seen *= std::clamp(num(QStringLiteral("opacity")), 0.0, 1.0);
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(c[i].x()) || !std::isfinite(c[i].y())) return false;
        out[i] = QPointF(c[i].x() * W, c[i].y() * H);
    }
    return true;
}

bool cornerPinMap(const QRect& area, const QPointF corners[4], Compose::Homography* out)
{
    if (area.isEmpty()) return false;
    double to[8];
    for (int i = 0; i < 4; ++i) to[2 * i] = corners[i].x(), to[2 * i + 1] = corners[i].y();
    Compose::Homography h;
    if (!Compose::homography(area.width(), area.height(), to, &h)) return false;
    // The content area's top left is the rectangle's origin: shift the source first
    Compose::Homography shift;
    shift.m[2] = -area.left();
    shift.m[5] = -area.top();
    *out = Compose::multiply(h, shift);
    return true;
}

namespace {

bool evalCornerPin(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input(QStringLiteral("src"));
    if (!src || src->kind != Buf::Rgba) return false;
    *out = ctx.take(Buf::Rgba);
    out->area = {};
    QPointF c[4];
    double seen = 1;
    Compose::Homography h;
    if (!cornerPinCorners(ctx.num, ctx.numOf, ctx.size, c, &seen) || seen <= 0) return true;
    // A crossed or folded quad: nothing (as a tilt edge-on)
    if (!cornerPinMap(src->contentArea().intersected(QRect(0, 0, src->w, src->h)), c, &h)) return true;
    Compose::draw(out->data(), out->w, out->h, src->data(), src->w, src->h, h, seen);
    return true;
}

} // namespace

void registerTrackNodes(QVector<NodeType>& out)
{
    NodeType k;
    k.id = QString::fromLatin1(::Comp::kTrackerType);
    k.name = T("Tracker");
    k.description = T("Verfolgt einen Punkt, zwei Punkte (Größe und Drehung), vier Ecken einer Fläche (Perspektive) "
                      "oder ein Gesicht; andere Knoten folgen ihm. Das Bild geht unverändert durch");
    k.inputs = {QStringLiteral("src")};
    EffectParam mode;
    mode.key = QStringLiteral("mode");
    mode.label = T("Verfolgen");
    mode.type = EffectParam::Choice;
    mode.choices = {QStringLiteral("point"), QStringLiteral("two"), QStringLiteral("face"), QStringLiteral("quad")};
    mode.choiceLabels = {T("Ein Punkt"), T("Zwei Punkte"), T("Gesicht"), T("Fläche (4 Ecken)")};
    mode.defaultValue = mode.choices.first();
    mode.description = T("Gesicht braucht die Erweiterung „Gesichtserkennung (YuNet)“");
    k.params << mode;
    k.params << number("ax", T("Punkt A x"), 0.5, 0, 1, 0.001, 3) << number("ay", T("Punkt A y"), 0.5, 0, 1, 0.001, 3)
             << number("bx", T("Punkt B x"), 0.6, 0, 1, 0.001, 3) << number("by", T("Punkt B y"), 0.5, 0, 1, 0.001, 3)
             << number("size", T("Suchfeld"), 0.05, 0.005, 0.5, 0.001, 3,
                       T("Seite des Quadrats, das um jeden Punkt gesucht wird (Anteil der Bildbreite)"))
             << number("x", T("Position x"), 0.5, -1, 2, 0.001, 3) << number("y", T("Position y"), 0.5, -1, 2, 0.001, 3)
             << number("scale", T("Größe"), 1, 0.01, 100, 0.01, 3) << number("angle", T("Drehung"), 0, -3600, 3600, 0.1, 2)
             << number("rx", T("Bezugspunkt x"), 0.5, -1, 2, 0.001, 3,
                       T("Wo das Verfolgte im Startbild ist; Folgende bewegen sich um den Abstand dazu"))
             << number("ry", T("Bezugspunkt y"), 0.5, -1, 2, 0.001, 3);
    // Mode quad: the start corners, clockwise from the top left (TrackStart's defaults)
    const double quad[8] = {0.3, 0.3, 0.7, 0.3, 0.7, 0.7, 0.3, 0.7};
    for (int i = 0; i < 4; ++i) {
        k.params << number(::Comp::kQuadStartKeys[2 * i], T("Ecke %1 x").arg(i + 1), quad[2 * i], 0, 1, 0.001, 3)
                 << number(::Comp::kQuadStartKeys[2 * i + 1], T("Ecke %1 y").arg(i + 1), quad[2 * i + 1], 0, 1, 0.001, 3);
    }
    EffectParam lost;
    lost.key = QString::fromLatin1(::Comp::kLostKey);
    lost.label = T("Bei Verlust");
    lost.type = EffectParam::Choice;
    // auto (also a missing value: older projects, recipes): hold for a face - a blur over it must never fade and show
    // it - and fade for points (::Comp::lostModeOf)
    lost.choices = {QStringLiteral("auto"), QStringLiteral("fade"), QStringLiteral("hold"), QStringLiteral("bridge")};
    lost.choiceLabels = {T("Automatisch"), T("Ausblenden"), T("Halten"), T("Überbrücken")};
    lost.defaultValue = lost.choices.first();
    lost.description = T("Was Folgende tun, solange der Punkt verloren ist: weich aus- und wieder einblenden, an der "
                         "letzten sicheren Stelle stehen bleiben oder die Lücke überbrücken. Lücken bis %1 Bilder "
                         "werden immer überbrückt. Automatisch: bei Gesichtern halten, sonst ausblenden")
                           .arg(::Comp::kBridgeFrames);
    k.params << lost;
    k.params << number(::Comp::kMaxLostKey, T("Aufgeben nach"), ::Comp::kDefaultMaxLost, 1, 10000, 1, 0,
                       T("So viele Bilder in Folge darf der Punkt verloren sein, dann endet die Spur (Analyse)"));
    EffectParam seen = number(::Comp::kSeenKey, T("Gesehen"), 1, 0, 1, 1, 0,
                              T("1 = gefunden, 0 = verloren (Keyframes der Analyse)"));
    seen.sparse = true; // only keyframes, written by the analysis
    k.params << seen;
    // Mode quad: the corners found (keyframes of the analysis; not analyzed = the start corners)
    for (int i = 0; i < 8; ++i) {
        EffectParam c = number(::Comp::kCornerKeys[i], T("Ecke %1 %2 (Ergebnis)").arg(i / 2 + 1).arg(i % 2 ? 'y' : 'x'),
                               quad[i], -1, 2, 0.001, 3);
        c.sparse = true;
        k.params << c;
    }
    k.output = Buf::Rgba;
    k.varying = false; // the picture is its input's; the results are keyframes the engine sees
    k.passThrough = QStringLiteral("src"); // the engine hands the picture on without a copy
    k.eval = passThrough;
    out << k;

    NodeType pin;
    pin.id = QString::fromLatin1(kCornerPinType);
    pin.name = T("Auf Fläche legen");
    pin.description = T("Legt ein Bild oder Video perspektivisch auf vier Ecken (Bildschirm, Schild, Wand): von Hand "
                        "gezogen oder den Ecken eines Trackers im Modus „Fläche“ folgend");
    pin.inputs = {QStringLiteral("src")};
    for (int i = 0; i < 4; ++i)
        pin.params << number(::Comp::kCornerKeys[2 * i], T("Ecke %1 x").arg(i + 1), quad[2 * i], -1, 2, 0.001, 3)
                   << number(::Comp::kCornerKeys[2 * i + 1], T("Ecke %1 y").arg(i + 1), quad[2 * i + 1], -1, 2, 0.001, 3);
    pin.params << number("opacity", T("Deckkraft"), 1, 0, 1, 0.01, 2)
               << number("follow", T("Folgt Tracker"), 0, 0, 1e6, 1, 0,
                         T("Id eines Tracker-Knotens (0 = keiner): im Modus „Fläche“ nimmt er dessen Ecken, sonst "
                           "bewegen sich die eigenen Ecken mit dem Tracker"));
    pin.eval = evalCornerPin;
    out << pin;
}

} // namespace Picture::Comp
