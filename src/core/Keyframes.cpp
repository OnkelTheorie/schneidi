#include "core/Keyframes.h"

#include "core/EffectRegistry.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr Keys::ParamInfo kParams[] = {
    {AnimParam::ZoomX, "zoomX", false},         {AnimParam::ZoomY, "zoomY", false},
    {AnimParam::PosX, "posX", false},           {AnimParam::PosY, "posY", false},
    {AnimParam::Rotation, "rotation", false},   {AnimParam::CropLeft, "cropLeft", false},
    {AnimParam::CropRight, "cropRight", false}, {AnimParam::CropTop, "cropTop", false},
    {AnimParam::CropBottom, "cropBottom", false}, {AnimParam::Opacity, "opacity", false},
    {AnimParam::TitleSize, "titleSize", false}, {AnimParam::TitlePosX, "titlePosX", false},
    {AnimParam::TitlePosY, "titlePosY", false}, {AnimParam::TitleColor, "titleColor", true},
    {AnimParam::Volume, "volume", false},       {AnimParam::Pan, "pan", false},
    {AnimParam::FxBrightness, "fxBrightness", false}, {AnimParam::FxContrast, "fxContrast", false},
    {AnimParam::FxSaturation, "fxSaturation", false}, {AnimParam::FxTemp, "fxTemp", false},
    {AnimParam::FxTint, "fxTint", false},       {AnimParam::FxBlur, "fxBlur", false},
    {AnimParam::GradeLiftY, "gradeLiftY", false},     {AnimParam::GradeLiftR, "gradeLiftR", false},
    {AnimParam::GradeLiftG, "gradeLiftG", false},     {AnimParam::GradeLiftB, "gradeLiftB", false},
    {AnimParam::GradeGammaY, "gradeGammaY", false},   {AnimParam::GradeGammaR, "gradeGammaR", false},
    {AnimParam::GradeGammaG, "gradeGammaG", false},   {AnimParam::GradeGammaB, "gradeGammaB", false},
    {AnimParam::GradeGainY, "gradeGainY", false},     {AnimParam::GradeGainR, "gradeGainR", false},
    {AnimParam::GradeGainG, "gradeGainG", false},     {AnimParam::GradeGainB, "gradeGainB", false},
    {AnimParam::GradeOffsetY, "gradeOffsetY", false}, {AnimParam::GradeOffsetR, "gradeOffsetR", false},
    {AnimParam::GradeOffsetG, "gradeOffsetG", false}, {AnimParam::GradeOffsetB, "gradeOffsetB", false},
    {AnimParam::GradeContrast, "gradeContrast", false}, {AnimParam::GradePivot, "gradePivot", false},
    {AnimParam::GradeSaturation, "gradeSaturation", false}, {AnimParam::GradeTemp, "gradeTemp", false},
    {AnimParam::GradeTint, "gradeTint", false},       {AnimParam::GradeExposure, "gradeExposure", false},
};

// Verlauf zwischen zwei Keyframes (u = 0..1): Ease Out am ersten = langsam los, Ease In am zweiten = langsam an
double shape(const Keyframe& a, const Keyframe& b, double u)
{
    const bool slowStart = a.ease == KeyEase::EaseOut || a.ease == KeyEase::EaseInOut;
    const bool slowEnd = b.ease == KeyEase::EaseIn || b.ease == KeyEase::EaseInOut;
    if (slowStart && slowEnd) return (1.0 - std::cos(u * M_PI)) / 2.0;
    if (slowStart) return 1.0 - std::cos(u * M_PI / 2.0);
    if (slowEnd) return std::sin(u * M_PI / 2.0);
    return u;
}

double mix(double a, double b, double w, bool color)
{
    if (!color) return a + (b - a) * w;
    const QColor ca = Keys::toColor(a), cb = Keys::toColor(b);
    auto ch = [w](int x, int y) { return std::clamp(int(std::lround(x + (y - x) * w)), 0, 255); };
    return Keys::fromColor(QColor(ch(ca.red(), cb.red()), ch(ca.green(), cb.green()), ch(ca.blue(), cb.blue()),
                                  ch(ca.alpha(), cb.alpha())));
}

bool inList(const QVector<AnimParam>& params, AnimParam p) { return params.isEmpty() || params.contains(p); }

void sortTrack(KeyTrack& k)
{
    std::sort(k.begin(), k.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
}

} // namespace

namespace Keys {

const ParamInfo& info(AnimParam p)
{
    for (const auto& i : kParams)
        if (i.param == p) return i;
    return kParams[0];
}

bool fromId(const QString& id, AnimParam* p)
{
    for (const auto& i : kParams)
        if (id == QLatin1String(i.id)) {
            *p = i.param;
            return true;
        }
    return false;
}

double staticValue(const Clip& c, AnimParam p)
{
    const ClipTransform& t = c.transform;
    switch (p) {
    case AnimParam::ZoomX: return t.zoomX;
    case AnimParam::ZoomY: return t.zoomY;
    case AnimParam::PosX: return t.posX;
    case AnimParam::PosY: return t.posY;
    case AnimParam::Rotation: return t.rotation;
    case AnimParam::CropLeft: return t.cropLeft;
    case AnimParam::CropRight: return t.cropRight;
    case AnimParam::CropTop: return t.cropTop;
    case AnimParam::CropBottom: return t.cropBottom;
    case AnimParam::Opacity: return t.opacity;
    case AnimParam::TitleSize: return c.title.size;
    case AnimParam::TitlePosX: return c.title.posX;
    case AnimParam::TitlePosY: return c.title.posY;
    case AnimParam::TitleColor: return fromColor(c.title.color);
    case AnimParam::Volume: return c.volumeDb;
    case AnimParam::Pan: return c.pan;
    default: break;
    }
    // Effekt-Parameter: Wert steht in der Effekt-Instanz (fehlt der Effekt: Default)
    const EffectDescriptor* e = nullptr;
    const EffectParam* ep = nullptr;
    if (EffectRegistry::paramFor(p, &e, &ep)) return EffectRegistry::value(c, e->id, ep->key).toDouble();
    return 0;
}

void setStaticValue(Clip& c, AnimParam p, double v)
{
    ClipTransform& t = c.transform;
    switch (p) {
    case AnimParam::ZoomX: t.zoomX = v; break;
    case AnimParam::ZoomY: t.zoomY = v; break;
    case AnimParam::PosX: t.posX = v; break;
    case AnimParam::PosY: t.posY = v; break;
    case AnimParam::Rotation: t.rotation = v; break;
    case AnimParam::CropLeft: t.cropLeft = v; break;
    case AnimParam::CropRight: t.cropRight = v; break;
    case AnimParam::CropTop: t.cropTop = v; break;
    case AnimParam::CropBottom: t.cropBottom = v; break;
    case AnimParam::Opacity: t.opacity = v; break;
    case AnimParam::TitleSize: c.title.size = v; break;
    case AnimParam::TitlePosX: c.title.posX = v; break;
    case AnimParam::TitlePosY: c.title.posY = v; break;
    case AnimParam::TitleColor: c.title.color = toColor(v); break;
    case AnimParam::Volume: c.volumeDb = v; break;
    case AnimParam::Pan: c.pan = v; break;
    default: {
        // Effekt-Parameter: nur in eine vorhandene Instanz schreiben (fehlt der Effekt, gibt es nichts zu ändern)
        const EffectDescriptor* e = nullptr;
        const EffectParam* ep = nullptr;
        if (EffectRegistry::paramFor(p, &e, &ep))
            if (EffectInstance* inst = EffectRegistry::instance(c, e->id)) inst->params[ep->key] = v;
        break;
    }
    }
}

bool animated(const Clip& c, AnimParam p)
{
    const auto it = c.keys.constFind(p);
    return it != c.keys.cend() && !it->isEmpty();
}

bool hasKeys(const Clip& c)
{
    for (const KeyTrack& k : c.keys)
        if (!k.isEmpty()) return true;
    return false;
}

double valueAt(const Clip& c, AnimParam p, double t)
{
    const auto it = c.keys.constFind(p);
    if (it == c.keys.cend() || it->isEmpty()) return staticValue(c, p);
    const KeyTrack& k = *it;
    const double s = c.in + t; // Quell-Frame
    if (s <= k.first().frame) return k.first().value;
    if (s >= k.last().frame) return k.last().value;
    for (int i = 1; i < k.size(); ++i) {
        if (s > k[i].frame) continue;
        const Keyframe& a = k[i - 1];
        const Keyframe& b = k[i];
        const double u = (s - a.frame) / double(b.frame - a.frame);
        return mix(a.value, b.value, shape(a, b, u), info(p).color);
    }
    return k.last().value;
}

const Keyframe* keyAt(const Clip& c, AnimParam p, int t)
{
    const auto it = c.keys.constFind(p);
    if (it == c.keys.cend()) return nullptr;
    for (const Keyframe& k : *it)
        if (k.frame == c.in + t) return &k;
    return nullptr;
}

void setKey(Clip& c, AnimParam p, int t, double v)
{
    KeyTrack& k = c.keys[p];
    for (Keyframe& x : k)
        if (x.frame == c.in + t) {
            x.value = v;
            return;
        }
    k << Keyframe{c.in + t, v, KeyEase::Linear};
    sortTrack(k);
}

void removeKey(Clip& c, AnimParam p, int t)
{
    auto it = c.keys.find(p);
    if (it == c.keys.end()) return;
    const double v = valueAt(c, p, t);
    it->erase(std::remove_if(it->begin(), it->end(), [&](const Keyframe& k) { return k.frame == c.in + t; }), it->end());
    if (it->isEmpty()) {
        c.keys.erase(it);
        setStaticValue(c, p, v); // wie DaVinci: der Wert bleibt stehen
    }
}

void setValue(Clip& c, AnimParam p, int t, double v)
{
    if (animated(c, p)) setKey(c, p, t, v);
    else setStaticValue(c, p, v);
}

void clear(Clip& c, AnimParam p)
{
    c.keys.remove(p);
}

QVector<int> keyTimes(const Clip& c, const QVector<AnimParam>& params)
{
    QVector<int> out;
    for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it) {
        if (!inList(params, it.key())) continue;
        for (const Keyframe& k : *it) out << k.frame - c.in;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool linearBetween(const Clip& c, AnimParam p, int t0, int t1)
{
    const auto it = c.keys.constFind(p);
    if (it == c.keys.cend() || it->size() < 2) return true;
    const KeyTrack& k = *it;
    const int s0 = c.in + t0, s1 = c.in + t1;
    for (int i = 1; i < k.size(); ++i) {
        const Keyframe& a = k[i - 1];
        const Keyframe& b = k[i];
        if (s1 <= a.frame || s0 >= b.frame) continue; // Abschnitt berührt dieses Segment nicht
        if (a.value == b.value) continue;
        if (shape(a, b, 0.5) != 0.5) return false;
    }
    return true;
}

void setEase(Clip& c, const QVector<int>& times, KeyEase ease, const QVector<AnimParam>& params)
{
    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
        if (!inList(params, it.key())) continue;
        for (Keyframe& k : *it)
            if (times.contains(k.frame - c.in)) k.ease = ease;
    }
}

void removeAt(Clip& c, const QVector<int>& times, const QVector<AnimParam>& params)
{
    const QList<AnimParam> keys = c.keys.keys();
    for (AnimParam p : keys) {
        if (!inList(params, p)) continue;
        for (int t : times) removeKey(c, p, t);
    }
}

void move(Clip& c, const QVector<int>& times, int delta)
{
    if (delta == 0) return;
    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
        KeyTrack moved, kept;
        for (const Keyframe& k : *it) {
            if (times.contains(k.frame - c.in)) {
                Keyframe m = k;
                m.frame += delta;
                moved << m;
            } else {
                kept << k;
            }
        }
        if (moved.isEmpty()) continue;
        // Verschobene Keyframes gewinnen gegen solche, die am Ziel schon liegen
        kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const Keyframe& k) {
            return std::any_of(moved.begin(), moved.end(), [&](const Keyframe& m) { return m.frame == k.frame; });
        }), kept.end());
        *it = kept + moved;
        sortTrack(*it);
    }
}

void split(const Clip& original, Clip& left, Clip& right)
{
    // Quell-Frames bleiben gleich; links alles vor dem Schnitt, rechts alles ab dem Schnitt,
    // plus je ein Keyframe mit dem interpolierten Wert an der neuen Kante
    const int cut = right.in; // erster Quell-Frame des rechten Teils
    left.keys.clear();
    right.keys.clear();
    for (auto it = original.keys.cbegin(); it != original.keys.cend(); ++it) {
        if (it->isEmpty()) continue;
        KeyTrack l, r;
        for (const Keyframe& k : *it) (k.frame < cut ? l : r) << k;
        const double atLeftEnd = valueAt(original, it.key(), left.out - original.in);
        const double atCut = valueAt(original, it.key(), cut - original.in);
        if (!r.isEmpty() && (l.isEmpty() || l.last().frame != left.out)) l << Keyframe{left.out, atLeftEnd, KeyEase::Linear};
        if (!l.isEmpty() && r.first().frame != cut) r.prepend(Keyframe{cut, atCut, KeyEase::Linear});
        if (r.isEmpty()) r << Keyframe{cut, atCut, KeyEase::Linear};
        sortTrack(l);
        left.keys.insert(it.key(), l);
        right.keys.insert(it.key(), r);
    }
}

bool hasTransform(const Clip& c)
{
    if (!c.transform.transformOn) return false;
    if (c.transform.hasTransform()) return true;
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation})
        if (animated(c, p)) return true;
    return false;
}

bool hasCrop(const Clip& c)
{
    if (!c.transform.cropOn) return false;
    if (c.transform.hasCrop()) return true;
    for (AnimParam p : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom})
        if (animated(c, p)) return true;
    return false;
}

bool hasOpacity(const Clip& c)
{
    return c.transform.compositeOn && (c.transform.hasOpacity() || animated(c, AnimParam::Opacity));
}

} // namespace Keys
