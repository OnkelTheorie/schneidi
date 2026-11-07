// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package C (0.19): key, matte_tune
// - key: chroma key (the Green Screen effect's keyer with the same values: bit-identical to effect "chromakey") or
//   luma key (brightness range with a soft ramp); the result is the picture with the key in its alpha
// - matte_tune: finishing a matte (alpha of a picture or a mask): clip black/white (closes half-transparent holes in
//   the subject, clears a gray haze in the background), shrink/grow the edge, soften it, take out spill
#include "engine/picture/CompNodes.h"

#include "core/I18n.h"
#include "engine/VideoEffects.h"
#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Picture::Comp {

namespace {

constexpr int kMinRows = 32; // rows per thread job (small pictures stay on one thread)

// The picture of `src` copied into a buffer for the node's result (pooled when it has the output size)
Buf copyOf(NodeCtx& ctx, const Buf& src)
{
    Buf out;
    if (src.w == ctx.size.width() && src.h == ctx.size.height()) out = ctx.take(src.kind);
    if (out.kind != src.kind || out.px.size() != src.px.size()) out = Buf(src.kind, src.w, src.h);
    std::memcpy(out.data(), src.data(), src.px.size());
    out.area = src.area;
    return out;
}

// fn(y0, y1) on bands of rows of the content area, spread over threads
void onRows(const Buf& b, const std::function<void(int y0, int y1)>& fn)
{
    const QRect a = b.contentArea().intersected(QRect(0, 0, b.w, b.h));
    if (a.isEmpty()) return;
    Compose::forRows(a.height(), kMinRows, [&](int r0, int r1) { fn(a.top() + r0, a.top() + r1); });
}

// dst (h rows of w) = src (w rows of h) transposed, in tiles (cache), bands of source rows on threads
void transposePlane(const uint8_t* src, uint8_t* dst, int w, int h)
{
    constexpr int B = 32;
    Compose::forRows((h + B - 1) / B, 1, [&](int t0, int t1) {
        for (int y0 = t0 * B; y0 < std::min(h, t1 * B); y0 += B)
            for (int x0 = 0; x0 < w; x0 += B)
                for (int y = y0; y < std::min(h, y0 + B); ++y)
                    for (int x = x0; x < std::min(w, x0 + B); ++x) dst[size_t(x) * h + y] = src[size_t(y) * w + x];
    });
}

bool evalKey(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    Buf b = copyOf(ctx, *src);
    const int w = b.w;
    uint8_t* px = b.data();
    if (ctx.num("mode") >= 0.5) { // luma
        VideoEffects::LumaKeyParams p;
        p.low = ctx.num("low");
        p.high = ctx.num("high");
        p.soft = ctx.num("soft");
        p.invert = ctx.num("invert") > 0.5;
        onRows(b, [&](int y0, int y1) { VideoEffects::lumaKey(px + size_t(y0) * w * 4, w, y1 - y0, p); });
    } else { // chroma: the Green Screen effect's keyer (per pixel, so bands of rows give the same picture)
        VideoEffects::KeyParams p;
        p.color = ctx.color("color");
        p.tolerance = ctx.num("tolerance");
        p.softness = ctx.num("softness");
        p.spill = ctx.num("spill");
        onRows(b, [&](int y0, int y1) { VideoEffects::key(px + size_t(y0) * w * 4, w, y1 - y0, p); });
    }
    *out = std::move(b);
    return true;
}

bool evalMatteTune(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src) return false;
    Buf b = copyOf(ctx, *src);
    const int w = b.w, h = b.h;
    const bool rgba = b.kind == Buf::Rgba;

    const double black = std::clamp(ctx.num("black"), 0.0, 1.0), white = std::clamp(ctx.num("white"), 0.0, 1.0);
    const int grow = int(std::lround(std::clamp(ctx.num("grow"), -1000.0, 1000.0) * ctx.scale));
    const double sigma = std::max(0.0, ctx.num("soft") * ctx.scale);
    const double despill = rgba ? std::clamp(ctx.num("despill"), 0.0, 1.0) : 0.0;
    const bool levels = black > 0 || white < 1;
    const bool spatial = grow != 0 || sigma >= 0.5;

    // The matte as one plane: the mask itself or the alpha taken out of the picture (pooled scratch)
    Buf alpha;
    uint8_t* plane = b.data();
    if (rgba && (levels || spatial)) {
        if (w == ctx.size.width() && h == ctx.size.height()) alpha = ctx.take(Buf::Mask);
        if (alpha.kind != Buf::Mask || alpha.w != w || alpha.h != h) alpha = Buf(Buf::Mask, w, h);
        plane = alpha.data();
        const uint8_t* s = b.data();
        Compose::forRows(h, kMinRows, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w, e = size_t(y1) * w; i < e; ++i) plane[i] = s[i * 4 + 3];
        });
    }

    if (levels) { // clip black / white: a' = (a - black) / (white - black)
        uint8_t table[256];
        for (int v = 0; v < 256; ++v) {
            const double a = v / 255.0;
            const double o = white <= black ? (a >= white ? 1.0 : 0.0) : std::clamp((a - black) / (white - black), 0.0, 1.0);
            table[v] = uint8_t(o * 255 + 0.5);
        }
        Compose::forRows(h, kMinRows, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w, e = size_t(y1) * w; i < e; ++i) plane[i] = table[plane[i]];
        });
    }
    if (spatial) { // each operation: along the rows, then the same along the columns of the transposed plane
        Buf turned;
        if (w == ctx.size.width() && h == ctx.size.height()) turned = ctx.take(Buf::Mask);
        if (turned.kind != Buf::Mask || turned.px.size() != size_t(w) * h) turned = Buf(Buf::Mask, w, h);
        const auto separable = [&](const std::function<void(uint8_t* band, int width, int rows)>& rowsFn) {
            const auto pass = [&](uint8_t* p, int pw, int ph) {
                Compose::forRows(ph, kMinRows, [&](int y0, int y1) { rowsFn(p + size_t(y0) * pw, pw, y1 - y0); });
            };
            pass(plane, w, h);
            transposePlane(plane, turned.data(), w, h);
            pass(turned.data(), h, w);
            transposePlane(turned.data(), plane, h, w);
        };
        if (grow != 0) // grow/shrink before the blur (they do not commute)
            separable([&](uint8_t* p, int pw, int rows) { VideoEffects::matteMinMaxRows(p, pw, rows, std::abs(grow), grow > 0); });
        int radii[3];
        if (VideoEffects::matteBlurRadii(sigma, radii)) { // three boxes: rows on bands of rows, columns on bands of columns
            uint8_t* tmp = turned.data();
            constexpr int kCols = 256; // columns per band (contiguous in a row: the column pass vectorizes)
            for (const int r : radii) {
                Compose::forRows(h, kMinRows, [&](int y0, int y1) {
                    VideoEffects::matteBoxRows(plane + size_t(y0) * w, tmp + size_t(y0) * w, w, y1 - y0, r);
                });
                Compose::forRows((w + kCols - 1) / kCols, 1, [&](int b0, int b1) {
                    VideoEffects::matteBoxCols(tmp, plane, w, h, b0 * kCols, b1 * kCols, r);
                });
            }
        }
    }

    if (rgba && (levels || spatial)) {
        uint8_t* d = b.data();
        Compose::forRows(h, kMinRows, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w, e = size_t(y1) * w; i < e; ++i) d[i * 4 + 3] = plane[i];
        });
    }
    if (despill > 0) {
        const QColor key = ctx.color("color");
        uint8_t* d = b.data();
        Compose::forRows(h, kMinRows, [&](int y0, int y1) {
            VideoEffects::despill(d + size_t(y0) * w * 4, w, y1 - y0, key, despill);
        });
    }
    if (spatial) b.area = {}; // a grown or softened edge may reach out of the content rectangle
    *out = std::move(b);
    return true;
}

} // namespace

void registerKeyNodes(QVector<NodeType>& out)
{
    const QColor green(0, 255, 0);
    {
        NodeType k;
        k.id = "key";
        k.name = T("Key");
        k.description = T("Stellt frei: nach einer Farbe (Green Screen) oder nach der Helligkeit; das Ergebnis ist das "
                          "Bild mit dem Key im Alpha-Kanal");
        k.inputs = {"src"};
        EffectParam mode{"mode", T("Art"), {}, EffectParam::Choice, QString("chroma")};
        mode.choices = {"chroma", "luma"};
        mode.choiceLabels = {T("Farbe"), T("Helligkeit")};
        k.params = {
            mode,
            {"color", T("Farbe"), {}, EffectParam::Color, green, 0, 0},
            {"tolerance", T("Toleranz"), {}, EffectParam::Double, 0.3, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Wie weit eine Farbe von der Key-Farbe entfernt sein darf und noch durchsichtig wird")},
            {"softness", T("Weiche Kante"), {}, EffectParam::Double, 0.2, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Breite des Übergangs zwischen durchsichtig und sichtbar")},
            {"spill", T("Farbsaum entfernen"), {}, EffectParam::Double, 1.0, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Nimmt den Schimmer der Key-Farbe (grüner Rand, Widerschein) aus dem, was sichtbar bleibt")},
            {"low", T("Untere Grenze"), {}, EffectParam::Double, 0.1, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Luma-Key: was dunkler ist, wird durchsichtig")},
            {"high", T("Obere Grenze"), {}, EffectParam::Double, 1.0, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Luma-Key: was heller ist, wird durchsichtig")},
            {"soft", T("Weiche Kante"), {}, EffectParam::Double, 0.1, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Luma-Key: Breite des Übergangs an den Grenzen")},
            {"invert", T("Invertieren"), {}, EffectParam::Bool, false},
        };
        k.output = Buf::Rgba;
        k.eval = evalKey;
        out << k;
    }
    {
        NodeType k;
        k.id = "matte_tune";
        k.name = T("Matte verfeinern");
        k.description = T("Verfeinert eine Matte (Alpha eines Bildes oder eine Maske): Schwarz und Weiß abschneiden, "
                          "Kante schrumpfen oder wachsen lassen, weich machen, Farbsaum entfernen");
        k.inputs = {"src"};
        k.params = {
            {"black", T("Schwarz abschneiden"), {}, EffectParam::Double, 0.0, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Was durchsichtiger ist, wird ganz durchsichtig (Schleier im Hintergrund)")},
            {"white", T("Weiß abschneiden"), {}, EffectParam::Double, 1.0, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Was deckender ist, wird ganz deckend (schließt Löcher im Motiv)")},
            {"grow", T("Kante verschieben"), {}, EffectParam::Double, 0.0, -100.0, 100.0, AnimParam::Count, 0.1, 1, {},
             T("In Pixeln: positiv lässt die Matte wachsen, negativ schrumpfen")},
            {"soft", T("Kante weich"), {}, EffectParam::Double, 0.0, 0.0, 100.0, AnimParam::Count, 0.1, 1, {},
             T("In Pixeln: macht die Kante der Matte weich")},
            {"despill", T("Farbsaum entfernen"), {}, EffectParam::Double, 0.0, 0.0, 1.0, AnimParam::Count, 0.002, 3, {},
             T("Nimmt den Schimmer der Key-Farbe aus dem ganzen Bild (nur bei einem Bild, nicht bei einer Maske)")},
            {"color", T("Farbe"), {}, EffectParam::Color, green, 0, 0},
        };
        k.output = Buf::Rgba; // a mask at src gives a mask
        k.eval = evalMatteTune;
        out << k;
    }
}

} // namespace Picture::Comp
