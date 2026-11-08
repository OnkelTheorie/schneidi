// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kind "cutout" (0.19): a person cut out of the picture without a green screen ("text behind a
// person": the clip, a text, the cutout on top).
//
// The model does not run here: the analysis (engine/picture/CompCutout, op cutout_node) stores small mattes per clip
// frame in a cache file and records its id in the node's `cache` value. Per frame the node reads its matte, grows/
// softens it on the small grid, refines it to the full picture along the picture's own edges (guided filter, input
// `src` as the guide) and sets the edge: `threshold` = the person probability where the edge lies (a ramp of 0.5
// around it, so 0.5 makes a hazy model edge crisp). Not analysed (or a frame outside the analysed range) = nothing.
// Parameters: cache (the analysis id), threshold 0..1, soft and grow in project pixels, invert (the background
// instead), output picture (src with the matte as alpha) | mask.
#include "core/I18n.h"
#include "engine/picture/CompCutout.h"
#include "engine/picture/CompNodes.h"
#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Picture::Comp {

namespace {

constexpr int kMinRows = 32;
constexpr double kRamp = 0.5; // width of the edge ramp around threshold (probability)

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

bool evalCutout(NodeCtx& ctx, Buf* out)
{
    const QString id = ctx.text(QString::fromLatin1(kCutoutCacheKey));
    const std::shared_ptr<const MatteFile> file = mattesOf(id);
    if (!file) return false; // not analysed: nothing (cutoutHint says why)
    const int frame = int(std::floor(ctx.t + 0.5));
    std::vector<uint8_t> low = file->matte(frame);
    if (low.empty()) return false;
    const int w = ctx.size.width(), h = ctx.size.height();
    if (w <= 0 || h <= 0) return false;
    const Buf* src = ctx.input(QStringLiteral("src"));
    if (src && (src->kind != Buf::Rgba || src->w != w || src->h != h)) src = nullptr; // no usable guide
    const bool asMask = ctx.num(QStringLiteral("output")) >= 0.5;
    if (!asMask && !src) return false; // a picture needs its picture

    // Grow and soften on the small grid (project pixels -> output pixels -> small pixels)
    const double toLow = double(file->w) / w * ctx.scale;
    growMatte(low, file->w, file->h, ctx.num(QStringLiteral("grow")) * toLow);
    blurMatte(low, file->w, file->h, ctx.num(QStringLiteral("soft")) * toLow);
    if (ctx.cancelled()) return false;

    Buf m = ctx.take(Buf::Mask);
    if (m.w != w || m.h != h) m = Buf(Buf::Mask, w, h);
    refineMatte(low.data(), file->w, file->h, src ? src->data() : nullptr, w, h, m.data());

    // Edge ramp and invert as one table
    uint8_t lut[256];
    const double thr = ctx.num(QStringLiteral("threshold"));
    const bool invert = ctx.num(QStringLiteral("invert")) >= 0.5;
    for (int i = 0; i < 256; ++i) {
        const double v = std::clamp((i / 255.0 - thr) / kRamp + 0.5, 0.0, 1.0);
        lut[i] = uint8_t(std::lround((invert ? 1.0 - v : v) * 255.0));
    }
    uint8_t* mp = m.data();
    if (asMask) {
        Compose::forRows(h, kMinRows, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * w, e = size_t(y1) * w; i < e; ++i) mp[i] = lut[mp[i]];
        });
        *out = std::move(m);
        return true;
    }
    Buf b = ctx.take(Buf::Rgba);
    if (b.w != w || b.h != h) b = Buf(Buf::Rgba, w, h);
    const uint8_t* s = src->data();
    uint8_t* d = b.data();
    Compose::forRows(h, kMinRows, [&](int y0, int y1) {
        std::memcpy(d + size_t(y0) * w * 4, s + size_t(y0) * w * 4, size_t(y1 - y0) * w * 4);
        for (size_t i = size_t(y0) * w, e = size_t(y1) * w; i < e; ++i)
            d[4 * i + 3] = uint8_t((d[4 * i + 3] * lut[mp[i]] + 127) / 255);
    });
    b.area = src->area;
    if (ctx.give) ctx.give(std::move(m));
    *out = std::move(b);
    return true;
}

} // namespace

void registerCutoutNodes(QVector<NodeType>& out)
{
    NodeType k;
    k.id = QString::fromLatin1(kCutoutType);
    k.name = T("Person freistellen");
    k.description = T("Stellt eine Person ohne Greenscreen frei (z. B. Text hinter einer Person). Braucht einmal "
                      "„Freistellen analysieren“ (CLI: cutout_node) und die Erweiterung „Freistellen (Personen)“; "
                      "ohne Analyse zeigt der Knoten nichts");
    k.inputs = {QStringLiteral("src")};
    EffectParam cache;
    cache.key = QString::fromLatin1(kCutoutCacheKey);
    cache.label = T("Analyse");
    cache.type = EffectParam::Path; // free text: the analysis id (no file key, see core/CompGraph.cpp kFileKeys)
    cache.defaultValue = QString();
    cache.description = T("Kennung der Freistell-Analyse; wird von der Analyse gesetzt");
    k.params << cache;
    k.params << number("threshold", T("Schwelle"), 0.5, 0, 1, 0.01, 2,
                       T("Ab welcher Sicherheit ein Pixel zur Person gehört (die Kante liegt hier)"))
             << number("soft", T("Weiche Kante"), 0, 0, 100, 0.5, 1, T("Kante weicher machen (Pixel)"))
             << number("grow", T("Wachsen"), 0, -50, 50, 0.5, 1,
                       T("Umriss vergrößern (positiv) oder verkleinern (negativ), in Pixeln"));
    EffectParam invert;
    invert.key = QStringLiteral("invert");
    invert.label = T("Umkehren");
    invert.type = EffectParam::Bool;
    invert.defaultValue = false;
    invert.description = T("Den Hintergrund statt der Person behalten");
    k.params << invert;
    EffectParam output;
    output.key = QStringLiteral("output");
    output.label = T("Ausgabe");
    output.type = EffectParam::Choice;
    output.choices = {QStringLiteral("picture"), QStringLiteral("mask")};
    output.choiceLabels = {T("Bild mit Transparenz"), T("Maske")};
    output.defaultValue = output.choices.first();
    k.params << output;
    k.output = Buf::Rgba; // or a mask (output = mask)
    k.varying = true;     // a matte per frame, without keyframes
    k.eval = evalCutout;
    out << k;
}

} // namespace Picture::Comp
