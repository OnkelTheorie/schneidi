#include "core/Keyframes.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kBig = 1e9;
constexpr Keys::ParamInfo kParams[] = {
    {AnimParam::ZoomX, "zoomX", false, N_("Zoom X"), 0, 100},
    {AnimParam::ZoomY, "zoomY", false, N_("Zoom Y"), 0, 100},
    {AnimParam::PosX, "posX", false, N_("Position X"), -kBig, kBig},
    {AnimParam::PosY, "posY", false, N_("Position Y"), -kBig, kBig},
    {AnimParam::Rotation, "rotation", false, N_("Rotation"), -kBig, kBig},
    {AnimParam::CropLeft, "cropLeft", false, N_("Beschneiden links"), 0, kBig},
    {AnimParam::CropRight, "cropRight", false, N_("Beschneiden rechts"), 0, kBig},
    {AnimParam::CropTop, "cropTop", false, N_("Beschneiden oben"), 0, kBig},
    {AnimParam::CropBottom, "cropBottom", false, N_("Beschneiden unten"), 0, kBig},
    {AnimParam::Opacity, "opacity", false, N_("Deckkraft"), 0, 100},
    {AnimParam::TitleSize, "titleSize", false, N_("Textgröße"), 1, 400},
    {AnimParam::TitlePosX, "titlePosX", false, N_("Text Position X"), -kBig, kBig},
    {AnimParam::TitlePosY, "titlePosY", false, N_("Text Position Y"), -kBig, kBig},
    {AnimParam::TitleColor, "titleColor", true, N_("Textfarbe"), 0, 0},
    {AnimParam::Volume, "volume", false, N_("Lautstärke"), kMinVolumeDb, kMaxVolumeDb},
    {AnimParam::Pan, "pan", false, "Pan", -100, 100},
    // Effekt-Parameter: Name und Bereich aus der EffectRegistry
    {AnimParam::FxBrightness, "fxBrightness", false, nullptr, 0, 0},
    {AnimParam::FxContrast, "fxContrast", false, nullptr, 0, 0},
    {AnimParam::FxSaturation, "fxSaturation", false, nullptr, 0, 0},
    {AnimParam::FxTemp, "fxTemp", false, nullptr, 0, 0},
    {AnimParam::FxTint, "fxTint", false, nullptr, 0, 0},
    {AnimParam::FxBlur, "fxBlur", false, nullptr, 0, 0},
    {AnimParam::GradeLiftY, "gradeLiftY", false, nullptr, 0, 0},
    {AnimParam::GradeLiftR, "gradeLiftR", false, nullptr, 0, 0},
    {AnimParam::GradeLiftG, "gradeLiftG", false, nullptr, 0, 0},
    {AnimParam::GradeLiftB, "gradeLiftB", false, nullptr, 0, 0},
    {AnimParam::GradeGammaY, "gradeGammaY", false, nullptr, 0, 0},
    {AnimParam::GradeGammaR, "gradeGammaR", false, nullptr, 0, 0},
    {AnimParam::GradeGammaG, "gradeGammaG", false, nullptr, 0, 0},
    {AnimParam::GradeGammaB, "gradeGammaB", false, nullptr, 0, 0},
    {AnimParam::GradeGainY, "gradeGainY", false, nullptr, 0, 0},
    {AnimParam::GradeGainR, "gradeGainR", false, nullptr, 0, 0},
    {AnimParam::GradeGainG, "gradeGainG", false, nullptr, 0, 0},
    {AnimParam::GradeGainB, "gradeGainB", false, nullptr, 0, 0},
    {AnimParam::GradeOffsetY, "gradeOffsetY", false, nullptr, 0, 0},
    {AnimParam::GradeOffsetR, "gradeOffsetR", false, nullptr, 0, 0},
    {AnimParam::GradeOffsetG, "gradeOffsetG", false, nullptr, 0, 0},
    {AnimParam::GradeOffsetB, "gradeOffsetB", false, nullptr, 0, 0},
    {AnimParam::GradeContrast, "gradeContrast", false, nullptr, 0, 0},
    {AnimParam::GradePivot, "gradePivot", false, nullptr, 0, 0},
    {AnimParam::GradeSaturation, "gradeSaturation", false, nullptr, 0, 0},
    {AnimParam::GradeTemp, "gradeTemp", false, nullptr, 0, 0},
    {AnimParam::GradeTint, "gradeTint", false, nullptr, 0, 0},
    {AnimParam::GradeExposure, "gradeExposure", false, nullptr, 0, 0},
};

bool slowStart(const Keyframe& k) { return k.ease == KeyEase::EaseOut || k.ease == KeyEase::EaseInOut; }
bool slowEnd(const Keyframe& k) { return k.ease == KeyEase::EaseIn || k.ease == KeyEase::EaseInOut; }

// Verlauf zwischen zwei Keyframes (u = 0..1): Ease Out am ersten = langsam los, Ease In am zweiten = langsam an
double shape(const Keyframe& a, const Keyframe& b, double u)
{
    if (slowStart(a) && slowEnd(b)) return (1.0 - std::cos(u * M_PI)) / 2.0;
    if (slowStart(a)) return 1.0 - std::cos(u * M_PI / 2.0);
    if (slowEnd(b)) return std::sin(u * M_PI / 2.0);
    return u;
}

// Bezier-Abschnitt (mindestens eine Seite Bezier): kubische Kurve durch (0, a.value) .. (D, b.value) in Frames/Wert.
// Seiten ohne Bezier bekommen einen abgeleiteten Griff: Ease = flach, sonst ein Drittel auf der Geraden
// (Linear–Linear ergibt so genau die Gerade).
bool bezierSegment(const Keyframe& a, const Keyframe& b) { return a.ease == KeyEase::Bezier || b.ease == KeyEase::Bezier; }

// Griff auf die Abschnittslänge kürzen, Steigung bleibt
void clampHandle(double& dt, double& dv, double maxAbs)
{
    if (std::abs(dt) <= maxAbs || dt == 0) return;
    dv *= maxAbs / std::abs(dt);
    dt = dt > 0 ? maxAbs : -maxAbs;
}

struct Controls {
    double x1, y1, x2, y2; // x relativ zu a (Frames), y = Wert
};

Controls controls(const Keyframe& a, const Keyframe& b)
{
    const double D = b.frame - a.frame;
    const double dv = b.value - a.value;
    Controls c{};
    if (a.ease == KeyEase::Bezier) {
        double dt = std::max(0.0, a.outDt), v = a.outDv;
        clampHandle(dt, v, D);
        c.x1 = dt;
        c.y1 = a.value + v;
    } else {
        c.x1 = D / 3;
        c.y1 = slowStart(a) ? a.value : a.value + dv / 3;
    }
    if (b.ease == KeyEase::Bezier) {
        double dt = std::min(0.0, b.inDt), v = b.inDv;
        clampHandle(dt, v, D);
        c.x2 = D + dt;
        c.y2 = b.value + v;
    } else {
        c.x2 = 2 * D / 3;
        c.y2 = slowEnd(b) ? b.value : b.value - dv / 3;
    }
    return c;
}

double cubic(double p0, double p1, double p2, double p3, double u)
{
    const double v = 1 - u;
    return v * v * v * p0 + 3 * v * v * u * p1 + 3 * v * u * u * p2 + u * u * u * p3;
}

// Kurvenparameter u zur Zeit x (Frames nach a). x(u) steigt monoton, weil beide Griffe im Abschnitt liegen.
double solveU(const Controls& c, double D, double x)
{
    if (x <= 0) return 0;
    if (x >= D) return 1;
    double lo = 0, hi = 1;
    for (int i = 0; i < 48; ++i) {
        const double mid = (lo + hi) / 2;
        (cubic(0, c.x1, c.x2, D, mid) < x ? lo : hi) = mid;
    }
    return (lo + hi) / 2;
}

double bezierValue(const Keyframe& a, const Keyframe& b, double x)
{
    const Controls c = controls(a, b);
    const double D = b.frame - a.frame;
    return cubic(a.value, c.y1, c.y2, b.value, solveU(c, D, x));
}

// Abgeleiteter Griff eines Keyframes ohne Bezier (relativ), passend zu controls()
void derivedHandle(const KeyTrack& k, int i, bool out, double* dt, double* dv)
{
    *dt = *dv = 0;
    const int j = out ? i + 1 : i - 1;
    if (j < 0 || j >= k.size()) return;
    const double D = std::abs(k[j].frame - k[i].frame);
    *dt = out ? D / 3 : -D / 3;
    const bool flat = out ? slowStart(k[i]) : slowEnd(k[i]);
    *dv = flat ? 0 : (k[j].value - k[i].value) / 3;
}

// Teilen in einem Bezier-Abschnitt: Kurve an den neuen Kanten zerlegen (de Casteljau), damit beide Teile
// genau der alten Kurve folgen. l endet mit dem Keyframe an leftEnd, r beginnt mit dem an cut.
void splitBezier(const KeyTrack& k, int cut, int leftEnd, KeyTrack& l, KeyTrack& r)
{
    int i = 1;
    while (i < k.size() && k[i].frame < cut) ++i;
    if (i <= 0 || i >= k.size()) return;
    const Keyframe& a = k[i - 1];
    const Keyframe& b = k[i];
    if (!bezierSegment(a, b)) return;
    const Controls c = controls(a, b);
    const double D = b.frame - a.frame;
    struct P {
        double x, y;
    };
    auto lerp = [](P p, P q, double u) { return P{p.x + (q.x - p.x) * u, p.y + (q.y - p.y) * u}; };
    const P p0{0, a.value}, p1{c.x1, c.y1}, p2{c.x2, c.y2}, p3{D, b.value};
    // Seiten ohne Bezier werden Bezier mit ihrer bisherigen Form als Griff
    auto toBezier = [&](Keyframe& key, int idx) {
        if (key.ease == KeyEase::Bezier) return;
        derivedHandle(k, idx, false, &key.inDt, &key.inDv);
        derivedHandle(k, idx, true, &key.outDt, &key.outDv);
        key.ease = KeyEase::Bezier;
    };
    if (l.size() >= 2 && l.last().frame == leftEnd && l[l.size() - 2].frame == a.frame) {
        const double u = solveU(c, D, leftEnd - a.frame);
        const P q1 = lerp(p0, p1, u), m = lerp(p1, p2, u), q2 = lerp(q1, m, u);
        const P q3 = lerp(q2, lerp(m, lerp(p2, p3, u), u), u);
        Keyframe& ka = l[l.size() - 2];
        toBezier(ka, i - 1);
        ka.outDt = q1.x;
        ka.outDv = q1.y - a.value;
        Keyframe& kl = l.last();
        kl.ease = KeyEase::Bezier;
        kl.inDt = q2.x - q3.x;
        kl.inDv = q2.y - q3.y;
        kl.outDt = -kl.inDt;
        kl.outDv = -kl.inDv;
    }
    if (r.size() >= 2 && r.first().frame == cut && r[1].frame == b.frame) {
        const double u = solveU(c, D, cut - a.frame);
        const P m = lerp(p1, p2, u), r2 = lerp(p2, p3, u), r1 = lerp(m, r2, u);
        const P r0 = lerp(lerp(lerp(p0, p1, u), m, u), r1, u);
        Keyframe& kr = r.first();
        kr.ease = KeyEase::Bezier;
        kr.outDt = r1.x - r0.x;
        kr.outDv = r1.y - r0.y;
        kr.inDt = -kr.outDt;
        kr.inDv = -kr.outDv;
        Keyframe& kb = r[1];
        toBezier(kb, i);
        kb.inDt = r2.x - D;
        kb.inDv = r2.y - b.value;
    }
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

QString label(AnimParam p)
{
    const ParamInfo& i = info(p);
    if (i.name) return T(i.name);
    const EffectDescriptor* e = nullptr;
    const EffectParam* ep = nullptr;
    if (EffectRegistry::paramFor(p, &e, &ep)) return QStringLiteral("%1: %2").arg(e->name, ep->label);
    return QString::fromLatin1(i.id);
}

void range(AnimParam p, double* lo, double* hi)
{
    const ParamInfo& i = info(p);
    *lo = i.min;
    *hi = i.max;
    const EffectDescriptor* e = nullptr;
    const EffectParam* ep = nullptr;
    if (!i.name && EffectRegistry::paramFor(p, &e, &ep)) {
        *lo = ep->min;
        *hi = ep->max;
    }
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
        const bool color = info(p).color;
        if (bezierSegment(a, b) && !color) return bezierValue(a, b, s - a.frame);
        // Farben: Bezier-Seiten wie linear (Griffe haben keinen Farbwert)
        auto plain = [](Keyframe k) {
            if (k.ease == KeyEase::Bezier) k.ease = KeyEase::Linear;
            return k;
        };
        return mix(a.value, b.value, shape(plain(a), plain(b), u), color);
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
        if (bezierSegment(a, b) && !info(p).color) {
            // Bezier: nur eine waagerechte Kurve ist sicher linear (sonst Frame für Frame rechnen)
            const Controls ctl = controls(a, b);
            if (a.value == b.value && ctl.y1 == a.value && ctl.y2 == a.value) continue;
            return false;
        }
        if (a.value == b.value) continue;
        if (shape(a, b, 0.5) != 0.5) return false;
    }
    return true;
}

void rescaleHandles(const KeyTrack& before, KeyTrack& after)
{
    const int n = std::min(before.size(), after.size());
    for (int i = 0; i < n; ++i) {
        if (after[i].ease != KeyEase::Bezier) continue;
        auto ratio = [&](int j) {
            const double o = before[j].frame - before[i].frame, nw = after[j].frame - after[i].frame;
            return o != 0 ? nw / o : 1.0;
        };
        const double rPrev = i > 0 ? ratio(i - 1) : i + 1 < n ? ratio(i + 1) : 1.0;
        const double rNext = i + 1 < n ? ratio(i + 1) : rPrev;
        // nur die Zeit streckt sich, der Wert des Griffs bleibt; rückwärts tauschen ein und aus
        const double inDt = before[i].inDt * rPrev, inDv = before[i].inDv;
        const double outDt = before[i].outDt * rNext, outDv = before[i].outDv;
        const bool flip = rPrev < 0 || rNext < 0;
        after[i].inDt = flip ? outDt : inDt;
        after[i].inDv = flip ? outDv : inDv;
        after[i].outDt = flip ? inDt : outDt;
        after[i].outDv = flip ? inDv : outDv;
    }
}

void setEase(Clip& c, const QVector<int>& times, KeyEase ease, const QVector<AnimParam>& params)
{
    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
        if (!inList(params, it.key())) continue;
        KeyTrack& k = *it;
        QVector<int> fresh; // neu auf Bezier: weiche Griffe berechnen, wenn alle Verläufe stehen
        for (int i = 0; i < k.size(); ++i)
            if (times.contains(k[i].frame - c.in)) {
                if (ease == KeyEase::Bezier && k[i].ease != KeyEase::Bezier) fresh << i;
                k[i].ease = ease;
            }
        for (int i : fresh) autoHandles(k, i);
    }
}

void autoHandles(KeyTrack& k, int i)
{
    if (i < 0 || i >= k.size()) return;
    Keyframe& key = k[i];
    const Keyframe* prev = i > 0 ? &k[i - 1] : nullptr;
    const Keyframe* next = i + 1 < k.size() ? &k[i + 1] : nullptr;
    // Steigung wie Catmull-Rom; an Spitzen/Tälern und an den Enden flach (kein Überschwingen)
    double slope = 0;
    if (prev && next && (key.value - prev->value) * (next->value - key.value) > 0)
        slope = (next->value - prev->value) / double(next->frame - prev->frame);
    const double inLen = prev ? (key.frame - prev->frame) / 3.0 : next ? (next->frame - key.frame) / 3.0 : 0;
    const double outLen = next ? (next->frame - key.frame) / 3.0 : inLen;
    key.inDt = -inLen;
    key.inDv = -inLen * slope;
    key.outDt = outLen;
    key.outDv = outLen * slope;
}

bool handle(const KeyTrack& k, int i, bool out, double* dt, double* dv)
{
    *dt = *dv = 0;
    if (i < 0 || i >= k.size()) return false;
    const int j = out ? i + 1 : i - 1;
    if (j < 0 || j >= k.size()) return false;
    const Keyframe& key = k[i];
    if (key.ease != KeyEase::Bezier) {
        derivedHandle(k, i, out, dt, dv);
        return true;
    }
    *dt = out ? std::max(0.0, key.outDt) : std::min(0.0, key.inDt);
    *dv = out ? key.outDv : key.inDv;
    clampHandle(*dt, *dv, std::abs(k[j].frame - key.frame));
    return true;
}

void setHandle(KeyTrack& k, int i, bool out, double dt, double dv, bool broken)
{
    if (i < 0 || i >= k.size()) return;
    Keyframe& key = k[i];
    if (key.ease != KeyEase::Bezier) { // bisherige Form beider Seiten als Griffe übernehmen
        derivedHandle(k, i, false, &key.inDt, &key.inDv);
        derivedHandle(k, i, true, &key.outDt, &key.outDv);
        if (i == 0) {
            key.inDt = -key.outDt;
            key.inDv = -key.outDv;
        }
        if (i + 1 == k.size()) {
            key.outDt = -key.inDt;
            key.outDv = -key.inDv;
        }
        key.ease = KeyEase::Bezier;
    }
    dt = out ? std::max(0.0, dt) : std::min(0.0, dt);
    (out ? key.outDt : key.inDt) = dt;
    (out ? key.outDv : key.inDv) = dv;
    if (broken || dt == 0) return;
    // wie DaVinci: Griffe bleiben auf einer Linie (gleiche Steigung), die Länge der anderen Seite bleibt
    const double slope = dv / dt;
    if (out) key.inDv = key.inDt * slope;
    else key.outDv = key.outDt * slope;
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
        const bool keysBefore = !l.isEmpty(); // sonst gilt rechts vor dem ersten Keyframe schon dessen Wert
        const double atLeftEnd = valueAt(original, it.key(), left.out - original.in);
        const double atCut = valueAt(original, it.key(), cut - original.in);
        if (!r.isEmpty() && (l.isEmpty() || l.last().frame != left.out)) l << Keyframe{left.out, atLeftEnd, KeyEase::Linear};
        if (r.isEmpty()) r << Keyframe{cut, atCut, KeyEase::Linear}; // alle Keyframes vor dem Schnitt
        else if (keysBefore && r.first().frame != cut) r.prepend(Keyframe{cut, atCut, KeyEase::Linear});
        sortTrack(l);
        if (!info(it.key()).color) splitBezier(*it, cut, left.out, l, r);
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
