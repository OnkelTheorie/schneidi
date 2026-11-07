// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package E (0.19): text, shadow, glitch
//
// text   - a title drawn like a title clip (core TitleStyle + TitleLook: font, colour, outline, box, fill gradient or
//          texture, 2.5D edge, shadow, glow), placed by the centre of its block (x, y 0..1 of the picture)
// shadow - a drop shadow from the alpha of `src`, laid under it (offset, soft, colour, opacity)
// glitch - digital glitch on `src`: RGB split, shifted blocks, jittered lines, noise; a new pattern every frame from
//          `seed` + the frame number (deterministic), strength 0 = the input unchanged
#include "engine/picture/CompNodes.h"

#include "core/I18n.h"
#include "core/Types.h"
#include "engine/picture/TitleLook.h"
#include "engine/video/Compose.h"
#include "engine/video/Text.h"

#include <QFontMetricsF>
#include <QImage>

#include <algorithm>
#include <cmath>

namespace Picture::Comp {

namespace {

// Parameter rows (label = German key, translated here)
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
EffectParam choice(const QString& key, const char* label, const QStringList& values, const QStringList& labels)
{
    EffectParam p = param(key, label, EffectParam::Choice, values.value(0));
    p.choices = values;
    p.choiceLabels = labels;
    return p;
}
// A free text: there is no text parameter type; Path keeps a string as it is (only the key "path" of a media node
// is treated as a file by the project file)
EffectParam text(const QString& key, const char* label, const QString& def)
{
    return param(key, label, EffectParam::Path, def);
}

// ---------- text ----------

QVector<EffectParam> textParams()
{
    const TitleStyle d;
    const TitleLook l;
    QVector<EffectParam> p{
        text("text", N_("Text"), QStringLiteral("Titel")),
        text("font", N_("Schriftart"), d.font),
        num("size", N_("Größe"), d.size, 1, 1000, 0.5, 1),
        color("color", N_("Farbe"), d.color),
        flag("bold", N_("Fett"), d.bold),
        flag("italic", N_("Kursiv"), d.italic),
        choice("align", N_("Ausrichtung"), {"center", "left", "right"},
               {T("Zentriert"), T("Linksbündig"), T("Rechtsbündig")}),
        num("x", N_("Position X"), 0.5, -1, 2, 0.001, 3),
        num("y", N_("Position Y"), 0.5, -1, 2, 0.001, 3),
        flag("outline", N_("Umrandung"), false),
        color("outline_color", N_("Umrandungsfarbe"), d.outlineColor),
        num("outline_width", N_("Breite"), d.outlineWidth, 0, 200, 0.1, 1),
        flag("box", N_("Hintergrund"), false),
        color("box_color", N_("Hintergrundfarbe"), d.boxColor),
        num("box_padding", N_("Abstand"), d.boxPad, 0, 500, 0.5, 1),
        choice("fill", N_("Füllung"), {"solid", "gradient", "texture"}, {T("Einfarbig"), T("Verlauf"), T("Textur")}),
        num("stops", N_("Verlaufsfarben"), 2, 2, 4, 1, 0),
        num("angle", N_("Verlaufswinkel"), l.gradAngle, -360, 360, 1, 0),
    };
    // Gradient stops c0..c3 at a0..a3 (a < 0 = evenly spaced); default the title colour to a darker one
    const QColor stopDefaults[4] = {d.color, d.color.darker(170), QColor(255, 210, 0), QColor(255, 60, 0)};
    for (int i = 0; i < 4; ++i) {
        p << color(QString("c%1").arg(i), N_("Verlaufsfarbe"), stopDefaults[i]);
        p << num(QString("a%1").arg(i), N_("Verlaufsstelle"), -1, -1, 1, 0.01, 2);
    }
    p << text("texture", N_("Textur"), {})
      << num("texture_scale", N_("Texturmaßstab"), l.texScale, 0.01, 100, 0.01, 2)
      << num("texture_x", N_("Textur X"), 0, -100000, 100000) << num("texture_y", N_("Textur Y"), 0, -100000, 100000)
      << flag("edge", N_("Kante"), false) << num("edge_depth", N_("Tiefe"), l.edgeDepth, 0, 500, 0.5, 1)
      << num("edge_angle", N_("Kantenwinkel"), l.edgeAngle, -360, 360, 1, 0)
      << color("edge_color", N_("Kantenfarbe"), l.edgeColor) << flag("edge_light", N_("Licht von oben"), l.edgeLight)
      << flag("shadow", N_("Schatten"), false) << num("shadow_x", N_("Schatten X"), l.shadowX, -1000, 1000, 0.5, 1)
      << num("shadow_y", N_("Schatten Y"), l.shadowY, -1000, 1000, 0.5, 1)
      << num("shadow_soft", N_("Weichheit"), l.shadowSoft, 0, 500, 0.5, 1)
      << color("shadow_color", N_("Schattenfarbe"), l.shadowColor)
      << num("shadow_opacity", N_("Deckkraft"), l.shadowOpacity, 0, 1, 0.01, 2) << flag("glow", N_("Glühen"), false)
      << num("glow_radius", N_("Radius"), l.glowRadius, 0, 500, 0.5, 1)
      << color("glow_color", N_("Glühfarbe"), l.glowColor)
      << num("glow_strength", N_("Stärke"), l.glowStrength, 0, 1, 0.01, 2);
    return p;
}

// The node's values as a title style (the same fields a title clip has)
TitleStyle styleOf(NodeCtx& ctx)
{
    TitleStyle s;
    s.text = ctx.text("text");
    s.font = ctx.text("font");
    s.size = std::max(1.0, ctx.num("size"));
    s.color = ctx.color("color");
    s.bold = ctx.num("bold") != 0;
    s.italic = ctx.num("italic") != 0;
    const int align = int(ctx.num("align")); // choice index: center, left, right
    s.align = align == 1 ? 0 : align == 2 ? 2 : 1;
    s.outlineOn = ctx.num("outline") != 0;
    s.outlineColor = ctx.color("outline_color");
    s.outlineWidth = std::max(0.0, ctx.num("outline_width"));
    s.boxOn = ctx.num("box") != 0;
    s.boxColor = ctx.color("box_color");
    s.boxPad = std::max(0.0, ctx.num("box_padding"));
    TitleLook& l = s.look;
    l.fill = TitleFill(std::clamp(int(ctx.num("fill")), 0, 2));
    const int stops = std::clamp(int(std::lround(ctx.num("stops"))), 2, 4);
    for (int i = 0; i < stops; ++i) {
        const double at = ctx.num(QString("a%1").arg(i));
        l.stops << ColorStop{at < 0 ? double(i) / (stops - 1) : std::min(1.0, at), ctx.color(QString("c%1").arg(i))};
    }
    l.gradAngle = ctx.num("angle");
    l.texture = ctx.text("texture");
    l.texScale = std::max(0.01, ctx.num("texture_scale"));
    l.texX = ctx.num("texture_x");
    l.texY = ctx.num("texture_y");
    l.edgeOn = ctx.num("edge") != 0;
    l.edgeDepth = std::max(0.0, ctx.num("edge_depth"));
    l.edgeAngle = ctx.num("edge_angle");
    l.edgeColor = ctx.color("edge_color");
    l.edgeLight = ctx.num("edge_light") != 0;
    l.shadowOn = ctx.num("shadow") != 0;
    l.shadowX = ctx.num("shadow_x");
    l.shadowY = ctx.num("shadow_y");
    l.shadowSoft = std::max(0.0, ctx.num("shadow_soft"));
    l.shadowColor = ctx.color("shadow_color");
    l.shadowOpacity = std::clamp(ctx.num("shadow_opacity"), 0.0, 1.0);
    l.glowOn = ctx.num("glow") != 0;
    l.glowRadius = std::max(0.0, ctx.num("glow_radius"));
    l.glowColor = ctx.color("glow_color");
    l.glowStrength = std::clamp(ctx.num("glow_strength"), 0.0, 1.0);
    return s;
}

QImage toImage(const Buf& b)
{
    if (b.isNull() || b.kind != Buf::Rgba) return {};
    return QImage(b.data(), b.w, b.h, b.stride(), QImage::Format_RGBA8888).copy();
}

bool evalText(NodeCtx& ctx, Buf* out)
{
    const TitleStyle st = styleOf(ctx);
    if (st.text.trimmed().isEmpty() || ctx.size.isEmpty()) return false;
    const double s = ctx.scale;
    QFont font(st.fontFamily());
    font.setPixelSize(std::max(1, int(std::lround(st.size * s))));
    font.setWeight(st.bold ? QFont::Bold : QFont::Normal);
    font.setItalic(st.italic);
    QFont measure(st.fontFamily()); // block width in project pixels, measured as a title clip measures it
    measure.setPixelSize(std::max(1, int(std::lround(st.size))));
    measure.setBold(st.bold);
    measure.setItalic(st.italic);
    const QFontMetricsF fm(measure);
    double w = 1;
    for (const QString& line : st.text.split('\n')) w = std::max(w, fm.horizontalAdvance(line));
    const bool outline = st.outlineOn && st.outlineWidth > 0;
    Text::Look look;
    const bool styled = st.look.active();
    if (styled) {
        QImage texture;
        if (st.look.fill == TitleFill::Texture && !st.look.texture.isEmpty() && ctx.mediaPicture)
            texture = toImage(ctx.mediaPicture(st.look.texture, ctx.t));
        look = titleLook(st.look, st.color, s, texture);
    }
    const double W = ctx.size.width(), H = ctx.size.height();
    const double cx = ctx.num("x") * W, cy = ctx.num("y") * H;
    *out = ctx.take(Buf::Rgba);
    QRect area;
    auto layer = [&](bool outlineLayer) {
        Text::Params p;
        if (styled) {
            p.look = &look;
            p.lookBehind = outlineLayer || !outline;
            p.lookFill = !outlineLayer;
        }
        p.text = st.text;
        p.area = QRectF(cx - w * s / 2, cy - H / 2, w * s, H);
        p.font = font;
        p.halign = st.align == 0 ? 0 : st.align == 2 ? 2 : 1;
        p.valign = 1;
        p.fg = outlineLayer ? st.outlineColor : st.color;
        p.bg = st.boxOn && (outlineLayer || !outline) ? st.boxColor : QColor(Qt::transparent);
        p.pad = (st.boxOn ? st.boxPad : 0.0) * s;
        p.outlineColor = st.outlineColor;
        p.outline = (outlineLayer ? 2 * st.outlineWidth : 0.0) * s;
        p.room = (outline ? 2 * st.outlineWidth : 0.0) * s;
        area |= Text::bounds(p).toAlignedRect();
        Text::draw(out->data(), out->w, out->h, p);
    };
    if (outline) layer(true);
    layer(false);
    out->area = area & QRect(0, 0, out->w, out->h);
    return !out->area.isEmpty();
}

// ---------- shadow ----------

bool evalShadow(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    const int w = src->w, h = src->h;
    const double opacity = std::clamp(ctx.num("opacity"), 0.0, 1.0);
    const QColor col = ctx.color("color");
    const int dx = int(std::lround(ctx.num("x") * ctx.scale)), dy = int(std::lround(ctx.num("y") * ctx.scale));
    const double soft = std::max(0.0, ctx.num("soft")) * ctx.scale;
    *out = ctx.take(Buf::Rgba);
    out->px = src->px;
    out->area = src->area;
    if (opacity <= 0 || col.alpha() == 0) return true; // no shadow: the input as it is
    // The shadow's plane: the input's alpha moved by the offset and blurred, only over the part it can reach
    const QRect content = src->contentArea();
    const int reach = int(std::ceil(soft)) + 1;
    const QRect sh = content.translated(dx, dy).adjusted(-reach, -reach, reach, reach) & QRect(0, 0, w, h);
    if (sh.isEmpty()) return true;
    const int pw = sh.width(), ph = sh.height();
    std::vector<uint8_t> plane(size_t(pw) * ph, 0);
    for (int y = 0; y < ph; ++y) {
        const int sy = sh.top() + y - dy;
        if (sy < 0 || sy >= h) continue;
        const uint8_t* row = src->data() + size_t(sy) * src->stride();
        uint8_t* dst = plane.data() + size_t(y) * pw;
        const int x0 = std::max(0, dx - sh.left()), x1 = std::min(pw, w + dx - sh.left());
        for (int x = x0; x < x1; ++x) dst[x] = row[(sh.left() + x - dx) * 4 + 3];
    }
    if (soft >= 1) Text::blurPlane(plane, pw, ph, soft);
    // src over the shadow (straight alpha)
    const int gain = int(std::lround(opacity * col.alpha() / 255.0 * 256)); // 0..256
    const int cr = col.red(), cg = col.green(), cb = col.blue();
    Compose::forRows(ph, 32, [&](int from, int to) {
        for (int y = from; y < to; ++y) {
            const uint8_t* pl = plane.data() + size_t(y) * pw;
            uint8_t* o = out->data() + size_t(sh.top() + y) * out->stride() + size_t(sh.left()) * 4;
            for (int x = 0; x < pw; ++x, o += 4) {
                const int ba = (pl[x] * gain) >> 8; // shadow alpha 0..255
                const int fa = o[3];
                if (!ba || fa == 255) continue;
                // alpha = fa + ba (1 - fa); colour = (f fa + b ba (1 - fa)) / alpha, all in units of 1/255
                const int bw = ba * (255 - fa);
                const int ao = fa * 255 + bw;
                o[0] = uint8_t((o[0] * fa * 255 + cr * bw + ao / 2) / ao);
                o[1] = uint8_t((o[1] * fa * 255 + cg * bw + ao / 2) / ao);
                o[2] = uint8_t((o[2] * fa * 255 + cb * bw + ao / 2) / ao);
                o[3] = uint8_t((ao + 127) / 255);
            }
        }
    });
    if (!src->area.isEmpty()) out->area = content | sh;
    return true;
}

// ---------- glitch ----------

// splitmix64 (public domain): a well mixed number from any 64-bit value
uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
struct Rng {
    uint64_t s;
    uint64_t next() { return s = mix(s); }
    double unit() { return double(next() >> 11) * (1.0 / 9007199254740992.0); } // 0..1
    double signedUnit() { return unit() * 2 - 1; }
};

bool evalGlitch(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    const double strength = std::clamp(ctx.num("strength"), 0.0, 1.0);
    *out = ctx.take(Buf::Rgba);
    if (strength <= 0) { // off: the input unchanged
        out->px = src->px;
        out->area = src->area;
        return true;
    }
    const int w = src->w, h = src->h;
    Rng rng{mix(uint64_t(std::llround(ctx.num("seed"))) * 0x100000001b3ull) ^ uint64_t(ctx.frameNo)};
    rng.next();
    // Horizontal shift of every row: a few shifted blocks (bands) plus single jittered lines
    std::vector<int> shift(size_t(h), 0);
    const double blocks = std::clamp(ctx.num("blocks"), 0.0, 1.0) * strength;
    const int bands = int(std::lround(blocks * 10));
    for (int i = 0; i < bands; ++i) {
        const int y0 = int(rng.unit() * h), bh = std::max(1, int((0.02 + 0.10 * rng.unit()) * h));
        const int dx = int(std::lround(rng.signedUnit() * 0.08 * blocks * w));
        for (int y = y0; y < std::min(h, y0 + bh); ++y) shift[y] += dx;
    }
    const double lines = std::clamp(ctx.num("lines"), 0.0, 1.0) * strength;
    if (lines > 0)
        for (int y = 0; y < h; ++y)
            if (rng.unit() < lines * 0.15) shift[y] += int(std::lround(rng.signedUnit() * 0.03 * w));
    const int split = int(std::lround(std::max(0.0, ctx.num("rgb")) * strength * ctx.scale));
    const int noise = int(std::lround(std::clamp(ctx.num("noise"), 0.0, 1.0) * strength * 80)); // +- per value
    const uint64_t noiseSeed = rng.next();
    Compose::forRows(h, 16, [&](int from, int to) {
        for (int y = from; y < to; ++y) {
            const uint8_t* row = src->data() + size_t(y) * src->stride();
            uint8_t* o = out->data() + size_t(y) * out->stride();
            const int d = shift[y];
            uint64_t n = mix(noiseSeed ^ uint64_t(y)) | 1;
            for (int x = 0; x < w; ++x, o += 4) {
                const int xg = std::min(w - 1, std::max(0, x - d));
                const int xr = std::min(w - 1, std::max(0, xg - split)), xb = std::min(w - 1, std::max(0, xg + split));
                const uint8_t* pr = row + xr * 4;
                const uint8_t* pg = row + xg * 4;
                const uint8_t* pb = row + xb * 4;
                // Split channels: each carries its own alpha (a red or blue ghost beside opaque letters shows)
                int r = pr[0], g = pg[1], b = pb[2];
                const int a = std::max({int(pr[3]), int(pg[3]), int(pb[3])});
                if (noise && a) {
                    n ^= n << 13;
                    n ^= n >> 7;
                    n ^= n << 17; // xorshift64
                    const int add = ((int(n & 0xff) - 128) * noise) >> 7;
                    r = std::min(255, std::max(0, r + add));
                    g = std::min(255, std::max(0, g + add));
                    b = std::min(255, std::max(0, b + add));
                }
                o[0] = uint8_t(r);
                o[1] = uint8_t(g);
                o[2] = uint8_t(b);
                o[3] = uint8_t(a);
            }
        }
    });
    out->area = QRect(); // rows may move anywhere: the whole picture
    return true;
}

} // namespace

void registerStyleNodes(QVector<NodeType>& out)
{
    NodeType text;
    text.id = "text";
    text.name = T("Text");
    text.description = T("Text wie ein Titel-Clip: Schrift, Farbe, Umrandung, Füllung mit Verlauf oder Textur, "
                         "3D-Kante, Schatten und Glühen");
    text.params = textParams();
    text.eval = evalText;
    out << text;

    NodeType shadow;
    shadow.id = "shadow";
    shadow.name = T("Schatten");
    shadow.description = T("Schlagschatten aus der Deckkraft des Eingangs, darunter gelegt");
    shadow.inputs = {"src"};
    shadow.params = {num("x", N_("Versatz X"), 6, -1000, 1000, 0.5, 1), num("y", N_("Versatz Y"), 6, -1000, 1000, 0.5, 1),
                     num("soft", N_("Weichheit"), 6, 0, 500, 0.5, 1), color("color", N_("Farbe"), Qt::black),
                     num("opacity", N_("Deckkraft"), 0.6, 0, 1, 0.01, 2)};
    shadow.eval = evalShadow;
    out << shadow;

    NodeType glitch;
    glitch.id = "glitch";
    glitch.name = T("Glitch");
    glitch.description = T("Digitale Störung: Farbversatz, verschobene Blöcke, Zeilen, Rauschen; jedes Bild neu aus "
                           "dem Startwert. Stärke 0 = aus");
    glitch.inputs = {"src"};
    glitch.params = {num("strength", N_("Stärke"), 0.3, 0, 1, 0.01, 2), num("rgb", N_("Farbversatz"), 8, 0, 200, 0.5, 1),
                     num("blocks", N_("Blöcke"), 0.5, 0, 1, 0.01, 2), num("lines", N_("Zeilen"), 0.5, 0, 1, 0.01, 2),
                     num("noise", N_("Rauschen"), 0.2, 0, 1, 0.01, 2), num("seed", N_("Startwert"), 1, 0, 1e6, 1, 0)};
    glitch.varying = true;
    glitch.eval = evalGlitch;
    out << glitch;
}

} // namespace Picture::Comp
