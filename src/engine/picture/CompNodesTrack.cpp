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
#include "core/CompTrack.h"
#include "core/I18n.h"
#include "engine/picture/CompNodes.h"

#include <algorithm>

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

void registerTrackNodes(QVector<NodeType>& out)
{
    NodeType k;
    k.id = QString::fromLatin1(::Comp::kTrackerType);
    k.name = T("Tracker");
    k.description = T("Verfolgt einen Punkt, zwei Punkte (Größe und Drehung) oder ein Gesicht; andere Knoten folgen "
                      "ihm. Das Bild geht unverändert durch");
    k.inputs = {QStringLiteral("src")};
    EffectParam mode;
    mode.key = QStringLiteral("mode");
    mode.label = T("Verfolgen");
    mode.type = EffectParam::Choice;
    mode.choices = {QStringLiteral("point"), QStringLiteral("two"), QStringLiteral("face")};
    mode.choiceLabels = {T("Ein Punkt"), T("Zwei Punkte"), T("Gesicht")};
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
    EffectParam lost;
    lost.key = QString::fromLatin1(::Comp::kLostKey);
    lost.label = T("Bei Verlust");
    lost.type = EffectParam::Choice;
    lost.choices = {QStringLiteral("fade"), QStringLiteral("hold"), QStringLiteral("bridge")};
    lost.choiceLabels = {T("Ausblenden"), T("Halten"), T("Überbrücken")};
    lost.defaultValue = lost.choices.first();
    lost.description = T("Was Folgende tun, solange der Punkt verloren ist: weich aus- und wieder einblenden, an der "
                         "letzten sicheren Stelle stehen bleiben oder die Lücke überbrücken. Lücken bis %1 Bilder "
                         "werden immer überbrückt")
                           .arg(::Comp::kBridgeFrames);
    k.params << lost;
    k.params << number(::Comp::kMaxLostKey, T("Aufgeben nach"), ::Comp::kDefaultMaxLost, 1, 10000, 1, 0,
                       T("So viele Bilder in Folge darf der Punkt verloren sein, dann endet die Spur (Analyse)"));
    EffectParam seen = number(::Comp::kSeenKey, T("Gesehen"), 1, 0, 1, 1, 0,
                              T("1 = gefunden, 0 = verloren (Keyframes der Analyse)"));
    seen.sparse = true; // only keyframes, written by the analysis
    k.params << seen;
    k.output = Buf::Rgba;
    k.varying = false; // the picture is its input's; the results are keyframes the engine sees
    k.passThrough = QStringLiteral("src"); // the engine hands the picture on without a copy
    k.eval = passThrough;
    out << k;
}

} // namespace Picture::Comp
