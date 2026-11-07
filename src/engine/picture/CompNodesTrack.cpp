// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package D (0.19): tracker
//
// tracker (input src): passes its picture through unchanged, so it can sit anywhere in a chain. What it tracks is
// measured once by the analysis (engine/picture/CompTrack, Editor::trackCompNode) and stored as keyframes of its own
// result parameters x, y, scale, angle; other nodes read them with numOf(id, "x") ("follow" = this node's id).
// Parameters: mode point|two|face; start points ax/ay (point, face: where the face is), bx/by (second point) 0..1;
// size = side of the square looked for, relative to the picture width; results x/y 0..1, scale (1 = as at the start),
// angle (degrees clockwise against the start); rx/ry = the reference point followers are drawn relative to (the
// follow rule: core/CompTrack.h).
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
    k.output = Buf::Rgba;
    k.varying = false; // the picture is its input's; the results are keyframes the engine sees
    k.passThrough = QStringLiteral("src"); // the engine hands the picture on without a copy
    k.eval = passThrough;
    out << k;
}

} // namespace Picture::Comp
