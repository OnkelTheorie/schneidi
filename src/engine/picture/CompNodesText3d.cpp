// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kind "text3d" (0.19): real 3D text. The letters' outlines (as a title lays them out) extruded
// into a mesh with an optional bevel, turned in space and drawn with perspective, one light and a simple material by
// engine/text3d. Every number is animatable (keyframes "comp.n<id>.<key>"); without keyframes the engine caches the
// picture (the node is not varying).
//
// Parameters: text, font, size, bold, italic, align, x/y (centre 0..1 of the picture), color (front), side_color
// (walls and bevel), depth, bevel (project pixels), rot_x/rot_y/rot_z (degrees), fov (degrees), distance (1 = the
// front at its 2D size), light_angle (degrees clockwise, -90 = from the top), light_height (degrees), ambient, shine,
// metal (0..1), aa (supersampling per axis 1..4).
#include "engine/picture/CompNodes.h"

#include "core/I18n.h"
#include "core/Types.h"
#include "engine/text3d/Text3d.h"
#include "engine/video/Text.h"

#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

namespace Picture::Comp {

namespace {

EffectParam param(const QString& key, const char* label, EffectParam::Type type, const QVariant& def)
{
    EffectParam p;
    p.key = key;
    p.label = T(label);
    p.type = type;
    p.defaultValue = def;
    return p;
}
EffectParam num(const QString& key, const char* label, double def, double lo, double hi, double step = 1,
                int decimals = 1)
{
    EffectParam p = param(key, label, EffectParam::Double, def);
    p.min = lo;
    p.max = hi;
    p.step = step;
    p.decimals = decimals;
    return p;
}
EffectParam color(const QString& key, const char* label, const QColor& def)
{
    return param(key, label, EffectParam::Color, def.name(QColor::HexArgb));
}
EffectParam flag(const QString& key, const char* label, bool def) { return param(key, label, EffectParam::Bool, def); }

QVector<EffectParam> text3dParams()
{
    const TitleStyle d;
    const Text3d::View v;
    EffectParam align = param("align", N_("Ausrichtung"), EffectParam::Choice, QStringLiteral("center"));
    align.choices = {"center", "left", "right"};
    align.choiceLabels = {T("Zentriert"), T("Linksbündig"), T("Rechtsbündig")};
    return {
        param("text", N_("Text"), EffectParam::Path, QStringLiteral("Titel")),
        param("font", N_("Schriftart"), EffectParam::Path, d.font),
        num("size", N_("Größe"), d.size, 1, 1000, 0.5, 1),
        flag("bold", N_("Fett"), true),
        flag("italic", N_("Kursiv"), d.italic),
        align,
        num("x", N_("Position X"), 0.5, -1, 2, 0.001, 3),
        num("y", N_("Position Y"), 0.5, -1, 2, 0.001, 3),
        color("color", N_("Farbe"), d.color),
        color("side_color", N_("Seitenfarbe"), v.side),
        num("depth", N_("Tiefe"), 30, 0, 1000, 0.5, 1),
        num("bevel", N_("Fase"), 2, 0, 100, 0.1, 1),
        num("rot_x", N_("Drehung X"), 15, -360, 360, 0.5, 1),
        num("rot_y", N_("Drehung Y"), -25, -360, 360, 0.5, 1),
        num("rot_z", N_("Drehung Z"), 0, -360, 360, 0.5, 1),
        num("fov", N_("Blickwinkel"), v.fov, 1, 150, 0.5, 1),
        num("distance", N_("Abstand"), v.distance, 0.1, 20, 0.01, 2),
        num("light_angle", N_("Lichtrichtung"), v.lightAngle, -360, 360, 1, 0),
        num("light_height", N_("Lichthöhe"), v.lightHeight, -90, 90, 1, 0),
        num("ambient", N_("Umgebungslicht"), v.ambient, 0, 1, 0.01, 2),
        num("shine", N_("Glanz"), v.shine, 0, 1, 0.01, 2),
        num("metal", N_("Metall"), v.metal, 0, 1, 0.01, 2),
        num("aa", N_("Kantenglättung"), v.aa, 1, 4, 1, 0),
    };
}

bool evalText3d(NodeCtx& ctx, Buf* out)
{
    const QString text = ctx.text("text");
    if (text.trimmed().isEmpty() || ctx.size.isEmpty()) return false;
    const double s = ctx.scale;
    TitleStyle st; // only for the font family rule (Sans on Windows)
    st.font = ctx.text("font");
    const double size = std::max(1.0, ctx.num("size"));
    QFont font(st.fontFamily());
    font.setPixelSize(std::max(1, int(std::lround(size * s))));
    font.setWeight(ctx.num("bold") != 0 ? QFont::Bold : QFont::Normal);
    font.setItalic(ctx.num("italic") != 0);
    const QFontMetricsF fm(font);
    double bw = 1;
    for (const QString& line : text.split('\n')) bw = std::max(bw, fm.horizontalAdvance(line));
    const double W = ctx.size.width(), H = ctx.size.height();
    const QPointF centre(ctx.num("x") * W, ctx.num("y") * H);
    Text::Params p;
    p.text = text;
    p.font = font;
    p.area = QRectF(centre.x() - bw / 2, centre.y() - H, bw, 2 * H); // the block centred on (x, y)
    const int align = int(ctx.num("align")); // choice index: center, left, right
    p.halign = align == 1 ? 0 : align == 2 ? 2 : 1;
    p.valign = 1;
    const QPainterPath path = Text::letters(p);
    if (ctx.cancelled()) return false;
    const double depth = std::max(0.0, ctx.num("depth")) * s;
    const Text3d::Mesh mesh = Text3d::extrude(path, depth, std::max(0.0, ctx.num("bevel")) * s);
    if (mesh.empty() || ctx.cancelled()) return false;
    Text3d::View v;
    v.rotX = ctx.num("rot_x");
    v.rotY = ctx.num("rot_y");
    v.rotZ = ctx.num("rot_z");
    v.pivot = centre;
    v.at = centre;
    v.depth = depth;
    v.fov = ctx.num("fov");
    v.distance = ctx.num("distance");
    v.lightAngle = ctx.num("light_angle");
    v.lightHeight = ctx.num("light_height");
    v.front = ctx.color("color");
    v.side = ctx.color("side_color");
    v.ambient = ctx.num("ambient");
    v.shine = ctx.num("shine");
    v.metal = ctx.num("metal");
    v.aa = int(std::lround(ctx.num("aa")));
    *out = ctx.take(Buf::Rgba);
    out->area = Text3d::render(mesh, v, out->data(), out->w, out->h);
    return !out->area.isEmpty();
}

} // namespace

void registerText3dNodes(QVector<NodeType>& out)
{
    NodeType k;
    k.id = "text3d";
    k.name = T("3D-Text");
    k.description = T("Echter 3D-Text: Buchstaben mit Tiefe und Fase, im Raum gedreht, mit Licht und Glanz; "
                      "Drehung, Tiefe, Kamera, Licht und Farben animierbar");
    k.params = text3dParams();
    k.eval = evalText3d;
    out << k;
}

} // namespace Picture::Comp
