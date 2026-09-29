#include "engine/TimelineBuilder.h"
#include "core/Loudness.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Profiles.h"

#include <Mlt.h>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

namespace {

// Keyframes (core/Keyframes.h) als MLT-Animation eines Filter-Werts über einen Ausschnitt: a = Clip-Frame am
// Anfang des Ausschnitts, len = seine Länge. Gesetzt wird an den Ausschnitt-Grenzen und an allen Keyframes der
// Parameter (MLT interpoliert linear dazwischen); Abschnitte mit Ease-Kurve (oder bake) Frame für Frame, damit
// Vorschau, Export und Inspector exakt dieselben Werte haben. false = keiner der Parameter animiert.
bool animate(const Clip& c, std::initializer_list<AnimParam> params, int a, int len,
             const std::function<void(int pos, double t)>& set, bool bake = false)
{
    if (len <= 0 || std::none_of(params.begin(), params.end(), [&](AnimParam p) { return Keys::animated(c, p); }))
        return false;
    QVector<int> pos{0, len - 1};
    for (int t : Keys::keyTimes(c, QVector<AnimParam>(params)))
        if (t - a > 0 && t - a < len - 1) pos << t - a;
    std::sort(pos.begin(), pos.end());
    pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
    for (int i = 0; i < pos.size(); ++i) {
        set(pos[i], a + pos[i]);
        if (i + 1 == pos.size()) break;
        bool linear = !bake;
        for (AnimParam p : params) linear = linear && Keys::linearBetween(c, p, a + pos[i], a + pos[i + 1]);
        if (!linear)
            for (int q = pos[i] + 1; q < pos[i + 1]; ++q) set(q, a + q);
    }
    return true;
}

// Filter an einen Ausschnitt hängen; Keyframes zählen ab Filter-In (siehe dev-notes)
void attachTo(Mlt::Producer& cut, Mlt::Filter& f)
{
    f.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(f);
}

// ---- Effekte (Open FX) ----

// Farbkorrektur als 3x3-Matrix (avfilter.colorchannelmixer): Weißabgleich wie DaVinci Temp/Tint als
// Kanal-Verstärkung, auf gleiche Helligkeit (Rec. 709) normiert, danach Sättigung (Mischung mit der Helligkeit).
// Temperatur +100 = warm (mehr Rot, weniger Blau), Tönung +100 = Magenta (weniger Grün), Sättigung -100 = grau.
struct Matrix { double m[3][3]; };
Matrix colorMatrix(double temp, double tint, double saturation)
{
    const double k = temp / 100.0 * 0.3, n = tint / 100.0 * 0.25;
    double g[3] = {1.0 + k, 1.0 - n, 1.0 - k};
    const double w[3] = {0.2126, 0.7152, 0.0722};
    const double l = w[0] * g[0] + w[1] * g[1] + w[2] * g[2];
    for (double& x : g) x /= l;
    const double s = std::max(0.0, 1.0 + saturation / 100.0);
    Matrix out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = std::clamp(((i == j ? s : 0.0) + (1.0 - s) * w[j]) * g[j], -2.0, 2.0);
    return out;
}

// Helligkeit/Kontrast als Tonwertkorrektur (avfilter.colorlevels): out = 0,5 + (in - 0,5)·c + b, begrenzt auf 0..1.
// colorlevels erlaubt Eingang -1..1 und Ausgang 0..1 -> Gerade dort anschneiden, wo sie 0 bzw. 1 erreicht.
struct Levels { double imin, imax, omin, omax; };
Levels levels(double brightness, double contrast)
{
    const double c = std::max(0.0, 1.0 + contrast / 100.0), b = brightness / 200.0;
    const double d = 0.5 - 0.5 * c + b; // out = c·in + d
    Levels lv{0.0, 1.0, d, c + d};
    if (lv.omin < 0 && c > 0) {
        lv.imin = std::min(-d / c, 1.0);
        lv.omin = 0;
    }
    if (lv.omax > 1 && c > 0) {
        lv.imax = std::max((1.0 - d) / c, -1.0);
        lv.omax = 1;
    }
    if (lv.imax - lv.imin < 1e-4) lv.imin = lv.imax - 1e-4; // ganz dunkel/hell: Sprung statt Division durch 0
    lv.omin = std::clamp(lv.omin, 0.0, 1.0);
    lv.omax = std::clamp(lv.omax, 0.0, 1.0);
    return lv;
}

// Wert eines Effekt-Parameters an Clip-Frame t (statisch aus der Instanz oder aus den Keyframes)
double fxValue(const Clip& c, AnimParam p, double t) { return Keys::valueAt(c, p, t); }

// Parameter nicht neutral bzw. animiert?
bool fxActive(const Clip& c, std::initializer_list<AnimParam> params, double neutral = 0.0)
{
    return std::any_of(params.begin(), params.end(),
                       [&](AnimParam p) { return Keys::animated(c, p) || Keys::staticValue(c, p) != neutral; });
}

// avfilter-Werte immer per anim_set (auch statisch: ein Wert am Anfang): als Zahl gesetzte Properties wandelt
// MLT beim Weiterreichen an FFmpeg in Text um – mit dem aktuellen Zahlenformat. Nur Filter mit Zahl-Optionen
// taugen (avfilter.eq hat Text-Optionen für Ausdrücke -> anim_set wirkt dort nicht).
void fxSet(const Clip& c, std::initializer_list<AnimParam> params, int a, int len,
           const std::function<void(int pos, double t)>& set)
{
    if (!animate(c, params, a, len, set)) set(0, a);
}

void applyColor(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, int a, int len)
{
    using P = AnimParam;
    if (fxActive(c, {P::FxTemp, P::FxTint, P::FxSaturation})) {
        Mlt::Filter f(profile, "avfilter.colorchannelmixer");
        if (f.is_valid()) {
            static const char* names[3][3] = {{"av.rr", "av.rg", "av.rb"}, {"av.gr", "av.gg", "av.gb"},
                                              {"av.br", "av.bg", "av.bb"}};
            fxSet(c, {P::FxTemp, P::FxTint, P::FxSaturation}, a, len, [&](int pos, double t) {
                const Matrix m = colorMatrix(fxValue(c, P::FxTemp, t), fxValue(c, P::FxTint, t),
                                             fxValue(c, P::FxSaturation, t));
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) f.anim_set(names[i][j], m.m[i][j], pos, len);
            });
            attachTo(cut, f);
        }
    }
    if (fxActive(c, {P::FxBrightness, P::FxContrast})) {
        Mlt::Filter f(profile, "avfilter.colorlevels");
        if (f.is_valid()) {
            fxSet(c, {P::FxBrightness, P::FxContrast}, a, len, [&](int pos, double t) {
                const Levels lv = levels(fxValue(c, P::FxBrightness, t), fxValue(c, P::FxContrast, t));
                for (const char* ch : {"r", "g", "b"}) {
                    f.anim_set(QByteArray("av.") + ch + "imin", lv.imin, pos, len);
                    f.anim_set(QByteArray("av.") + ch + "imax", lv.imax, pos, len);
                    f.anim_set(QByteArray("av.") + ch + "omin", lv.omin, pos, len);
                    f.anim_set(QByteArray("av.") + ch + "omax", lv.omax, pos, len);
                }
            });
            attachTo(cut, f);
        }
    }
}

void applyBlur(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, int a, int len)
{
    if (!fxActive(c, {AnimParam::FxBlur})) return;
    Mlt::Filter f(profile, "avfilter.gblur");
    if (!f.is_valid()) return;
    // Sigma relativ zur Profilhöhe (Stärke 100 = 1/20 der Bildhöhe); kleinere Vorschau rechnet MLT selbst um
    // (avformat/resolution_scale.yml), kleinerer Export hat ein kleineres Profil
    const double scale = profile.height() / 2000.0;
    fxSet(c, {AnimParam::FxBlur}, a, len, [&](int pos, double t) {
        const double sigma = std::max(0.0, fxValue(c, AnimParam::FxBlur, t)) * scale;
        f.anim_set("av.sigma", sigma, pos, len);
        // sigmaV = -1 (wie sigma) löst gblur nur beim Anlegen auf -> sonst bliebe die Senkrechte fast scharf
        f.anim_set("av.sigmaV", sigma, pos, len);
    });
    attachTo(cut, f);
}

// Vorher/Nachher-Schalter des gerade bauenden Builders (nur Vorschau), siehe TimelineBuilder::setGradeBypass
thread_local std::shared_ptr<std::atomic<bool>> t_gradeBypass;

// Effekte in der Reihenfolge am Clip; a/len: Ausschnitt für Keyframes.
// Farbkorrektur der Color-Seite zuerst (wie DaVinci: Grade vor den Edit-Effekten), unabhängig von der Position.
void applyEffects(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c, int a, int len)
{
    ColorGrade::attach(clip, c, a, t_gradeBypass);
    for (const auto& inst : c.effects) {
        if (!inst.enabled || inst.effectId == ColorGrade::EffectId) continue;
        if (inst.effectId == "color") {
            applyColor(profile, clip, c, a, len);
            continue;
        }
        if (inst.effectId == "blur") {
            applyBlur(profile, clip, c, a, len);
            continue;
        }
        const EffectDescriptor* d = EffectRegistry::find(inst.effectId);
        if (!d || d->mltService.isEmpty()) continue;
        Mlt::Filter f(profile, d->mltService.toUtf8().constData());
        if (!f.is_valid()) continue;
        for (const auto& p : d->params) {
            const QVariant v = inst.params.value(p.key, p.defaultValue);
            const QByteArray prop = p.mltProperty.toUtf8();
            if (p.type == EffectParam::Color) {
                const QColor col = v.value<QColor>();
                f.set(prop.constData(), col.name(QColor::HexRgb).toUtf8().constData()); // "#rrggbb"
            } else {
                f.set(prop.constData(), v.toDouble());
            }
        }
        clip.attach(f);
    }
}

double mltLevel(double db) { return db <= kMinVolumeDb ? -200.0 : db; }

// a/len: Ausschnitt für Keyframes (len = 0: nur statischer Wert, z. B. Mixer)
void applyVolume(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c, int a = 0, int len = 0)
{
    const bool anim = len > 0 && Keys::animated(c, AnimParam::Volume);
    if (c.volumeDb == 0.0 && !anim) return;
    Mlt::Filter f(profile, "volume");
    if (!f.is_valid()) return;
    // -∞ ist in MLT -200 dB -> Abschnitte, die dort anfangen/enden, nicht linear -> Frame für Frame
    const KeyTrack keys = c.keys.value(AnimParam::Volume);
    const bool bake = std::any_of(keys.begin(), keys.end(), [](const Keyframe& k) { return k.value <= kMinVolumeDb; });
    if (!anim || !animate(c, {AnimParam::Volume}, a, len, [&](int pos, double t) {
            f.anim_set("level", mltLevel(Keys::valueAt(c, AnimParam::Volume, t)), pos, len);
        }, bake))
        f.set("level", mltLevel(c.volumeDb)); // dB
    if (anim) attachTo(static_cast<Mlt::Producer&>(clip), f);
    else clip.attach(f);
}

// Seitenverhältnis des Quellbilds wie angezeigt (Pixel-Seitenverhältnis und Drehung aus den Metadaten), 0 = unbekannt
double sourceAspect(Mlt::Producer& cut)
{
    Mlt::Producer& p = cut.is_cut() ? cut.parent() : cut;
    double w = p.get_int("width"), h = p.get_int("height");
    if (w <= 0 || h <= 0) {
        w = p.get_int("meta.media.width");
        h = p.get_int("meta.media.height");
    }
    if (w <= 0 || h <= 0) return 0;
    if (const double sar = p.get_double("aspect_ratio"); sar > 0) w *= sar;
    const int vi = p.get_int("video_index");
    const int rotate = std::abs(p.get_int(QString("meta.media.%1.codec.rotate").arg(vi).toUtf8().constData())) % 180;
    return rotate == 90 ? h / w : w / h;
}

void applyTransform(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c, int a, int len)
{
    const double W = profile.width(), H = profile.height();
    auto val = [&](AnimParam p, double t) { return Keys::valueAt(c, p, t); };
    if (Keys::hasCrop(c)) {
        // Beschneiden = Rand transparent machen, Bild bleibt an seinem Platz (wie DaVinci)
        Mlt::Filter crop(profile, "qtcrop");
        if (crop.is_valid()) {
            auto rect = [&](double t) {
                const double l = val(AnimParam::CropLeft, t), r = val(AnimParam::CropRight, t);
                const double top = val(AnimParam::CropTop, t), b = val(AnimParam::CropBottom, t);
                return mlt_rect{l, top, std::max(1.0, W - l - r), std::max(1.0, H - top - b), 1.0};
            };
            if (!animate(c, {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom}, a, len,
                         [&](int pos, double t) { crop.anim_set("rect", rect(t), pos, len); }))
                crop.set("rect", rect(a));
            crop.set("color", "#00000000");
            attachTo(clip, crop);
        }
    }
    if (Keys::hasOpacity(c)) {
        // Deckkraft über den Alphakanal (wie Shotcut); die qtblend-Opacity lieferte nur Schwarz
        Mlt::Filter op(profile, "brightness");
        if (op.is_valid()) {
            op.set("level", 1.0);
            if (!animate(c, {AnimParam::Opacity}, a, len,
                         [&](int pos, double t) { op.anim_set("alpha", val(AnimParam::Opacity, t) / 100.0, pos, len); }))
                op.set("alpha", c.transform.opacity / 100.0);
            attachTo(clip, op);
        }
    }
    if (!Keys::hasTransform(c)) return;
    Mlt::Filter f(profile, "qtblend");
    if (!f.is_valid()) return;
    // distort=1 (unten) zieht die Quelle aufs Rechteck -> Rechteck selbst seitenverhältnis-treu ins Bild einpassen
    // (Hochformat-Clip im Querformat-Projekt bekommt seitlich Platz, wie ohne Transform und wie in DaVinci)
    double fitW = W, fitH = H;
    if (const double asp = c.isTitle() ? 0.0 : sourceAspect(clip); asp > 0) {
        if (asp > W / H) fitH = W / asp;
        else fitW = H * asp;
    }
    auto rect = [&](double t) {
        const double w = fitW * val(AnimParam::ZoomX, t), h = fitH * val(AnimParam::ZoomY, t);
        return mlt_rect{(W - w) / 2 + val(AnimParam::PosX, t), (H - h) / 2 - val(AnimParam::PosY, t), w, h, 1.0};
    };
    // Als mlt_rect statt Text setzen: MLT liest Text-Rechtecke mit dem System-Zahlenformat
    // (deutsch: "940.8" -> 940 und ".8" rutscht ins nächste Feld). 5. Wert (Deckkraft) muss 1 sein.
    if (!animate(c, {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY}, a, len,
                 [&](int pos, double t) { f.anim_set("rect", rect(t), pos, len); }))
        f.set("rect", rect(a));
    // DaVinci: positiv = gegen den Uhrzeigersinn
    if (!animate(c, {AnimParam::Rotation}, a, len,
                 [&](int pos, double t) { f.anim_set("rotation", -val(AnimParam::Rotation, t), pos, len); }))
        f.set("rotation", -c.transform.rotation);
    f.set("rotate_center", 1);
    // distort=1: Quelle in voller Größe holen und erst beim Zeichnen skalieren. Sonst fordert qtblend das Bild
    // in Zielhöhe an (bei kleinem Zoom nur ein paar Pixel -> Bildsalat) und hält X/Y-Zoom im Seitenverhältnis.
    f.set("distort", 1);
    attachTo(clip, f);
}

void applyPan(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c, int a = 0, int len = 0)
{
    const bool anim = len > 0 && Keys::animated(c, AnimParam::Pan);
    if (c.pan == 0.0 && !anim) return;
    Mlt::Filter f(profile, "panner");
    if (!f.is_valid()) return;
    f.set("channel", -1); // Balance (Stereo)
    // "start" statt "split": split wirkt in MLT 7 unabhängig vom Wert wie ein fester Versatz (getestet)
    f.set("start", (c.pan + 100.0) / 200.0);
    if (anim) {
        // animiert über "split" (per anim_set als Zahl gesetzt, nicht als Text -> Zahlenformat egal)
        animate(c, {AnimParam::Pan}, a, len, [&](int pos, double t) {
            f.anim_set("split", (Keys::valueAt(c, AnimParam::Pan, t) + 100.0) / 200.0, pos, len);
        });
        attachTo(static_cast<Mlt::Producer&>(clip), f);
    } else {
        clip.attach(f);
    }
}

// Mixer: Spur-/Master-Fader und -Pan als Filter auf Playlist bzw. Tractor.
// Mit hooks werden die Filter immer angehängt (neutral bei 0 dB/Mitte), damit sie live verstellbar sind.
double mltPan(double pan) { return (pan + 100.0) / 200.0; }

// Pegelmesser (passiv): merkt sich den Spitzenpegel des zuletzt verarbeiteten Tons je Kanal in dBFS als
// _audio_level.N (N = Kanal). Eigener Filter statt MLT "audiolevel": der wandelt den Ton nach s16 und schneidet
// damit alles über 0 dBFS ab (Vorschau klang anders als der Export, Übersteuerung war nicht messbar).
int meterGetAudio(mlt_frame frame, void** buffer, mlt_audio_format* format, int* frequency, int* channels, int* samples)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_audio(frame));
    const int error = mlt_frame_get_audio(frame, buffer, format, frequency, channels, samples);
    if (error || !*buffer || *channels <= 0 || *samples <= 0) return error;
    const int ch = std::min(*channels, 8), n = *samples;
    float peak[8] = {};
    auto sample = [&](int c, int i) -> float {
        switch (*format) {
        case mlt_audio_s16: return std::abs(static_cast<const int16_t*>(*buffer)[i * *channels + c] / 32768.f);
        case mlt_audio_s32le: return std::abs(static_cast<const int32_t*>(*buffer)[i * *channels + c] / 2147483648.f);
        case mlt_audio_s32: return std::abs(static_cast<const int32_t*>(*buffer)[c * n + i] / 2147483648.f);
        case mlt_audio_f32le: return std::abs(static_cast<const float*>(*buffer)[i * *channels + c]);
        case mlt_audio_float: return std::abs(static_cast<const float*>(*buffer)[c * n + i]);
        default: return 0.f;
        }
    };
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i) peak[c] = std::max(peak[c], sample(c, i));
    mlt_properties props = MLT_FILTER_PROPERTIES(filter);
    // Master in der Vorschau: Lautheit (Loudness-Meter im Mixer) mitmessen
    if (auto* live = static_cast<SharedLoudness*>(mlt_properties_get_data(props, "_loudness", nullptr));
        live && live->active) {
        auto signedSample = [&](int c, int i) -> float {
            switch (*format) {
            case mlt_audio_s16: return static_cast<const int16_t*>(*buffer)[i * *channels + c] / 32768.f;
            case mlt_audio_s32le: return static_cast<const int32_t*>(*buffer)[i * *channels + c] / 2147483648.f;
            case mlt_audio_s32: return static_cast<const int32_t*>(*buffer)[c * n + i] / 2147483648.f;
            case mlt_audio_f32le: return static_cast<const float*>(*buffer)[i * *channels + c];
            case mlt_audio_float: return static_cast<const float*>(*buffer)[c * n + i];
            default: return 0.f;
            }
        };
        std::lock_guard<std::mutex> lock(live->mutex);
        live->meter.add(n, *channels, *frequency, signedSample);
    }
    for (int c = 0; c < ch; ++c) {
        char name[32];
        std::snprintf(name, sizeof name, "_audio_level.%d", c);
        mlt_properties_set_double(props, name, peak[c] > 1e-10f ? 20.0 * std::log10(peak[c]) : -200.0);
    }
    return 0;
}

mlt_frame meterProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_audio(frame, filter);
    mlt_frame_push_audio(frame, reinterpret_cast<void*>(meterGetAudio));
    return frame;
}

std::shared_ptr<Mlt::Filter> makeMeter()
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = meterProcess;
    auto meter = std::make_shared<Mlt::Filter>(f); // hält eine eigene Referenz
    mlt_filter_close(f);
    return meter;
}

// Limiter am Master: avfilter.alimiter (Lookahead, Sample-Peak). av.level (Auto-Level) ist in FFmpeg standardmäßig an
// und würde alles auf die Ceiling hochziehen -> aus. Werte per anim_set (Zahlenformat, siehe dev-notes).
void setLimiter(Mlt::Filter& f, bool on, double ceilingDb)
{
    f.set("disable", on ? 0 : 1);
    f.anim_set("av.limit", std::clamp(std::pow(10.0, ceilingDb / 20.0), 0.0625, 1.0), 0);
}

std::shared_ptr<Mlt::Filter> makeLimiter(Mlt::Profile& profile, bool on, double ceilingDb)
{
    auto f = std::make_shared<Mlt::Filter>(profile, "avfilter.alimiter");
    if (!f->is_valid()) return nullptr;
    f->set("av.level", 0);
    f->set("av.asc", 0);
    f->anim_set("av.attack", 5.0, 0);
    f->anim_set("av.release", 50.0, 0);
    setLimiter(*f, on, ceilingDb);
    return f;
}

// limiter: nur Master (Ceiling in dBFS, nullopt = keiner); sitzt nach Fader/Pan und vor dem Pegelmesser
void attachStrip(Mlt::Profile& profile, Mlt::Service& s, double db, double pan, MixerHooks::Strip* hook,
                 std::optional<std::pair<bool, double>> limiter = {})
{
    if (!hook) { // Export: nur was nötig ist
        Clip c; // gleiche Umrechnung wie bei Clips
        c.volumeDb = db;
        c.pan = pan;
        applyVolume(profile, s, c);
        applyPan(profile, s, c);
        if (limiter && limiter->first)
            if (auto f = makeLimiter(profile, true, limiter->second)) s.attach(*f);
        return;
    }
    hook->volume = std::make_shared<Mlt::Filter>(profile, "volume");
    hook->volume->set("level", mltLevel(db));
    s.attach(*hook->volume);
    hook->pan = std::make_shared<Mlt::Filter>(profile, "panner");
    hook->pan->set("channel", -1);
    hook->pan->set("start", mltPan(pan));
    s.attach(*hook->pan);
    if (limiter) { // Vorschau: immer da, damit er sich live schalten lässt
        hook->limiter = makeLimiter(profile, limiter->first, limiter->second);
        if (hook->limiter) s.attach(*hook->limiter);
    }
    hook->meter = makeMeter();
    s.attach(*hook->meter);
}

// a/len: Ausschnitt (Clip-Frame am Anfang, Länge) für Keyframes
void decorate(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind, int a, int len)
{
    if (kind == TrackKind::Video) applyEffects(profile, cut, c, a, len);
    if (kind == TrackKind::Video) applyTransform(profile, cut, c, a, len);
    if (c.isTitle() && !Keys::hasTransform(c) && !Keys::animated(c, AnimParam::TitleSize)) {
        // qtext zeichnet in der angeforderten Größe, skaliert aber die Umrandung nicht mit (Vorschau 960 px:
        // Rand dreimal so dick). qtblend mit distort=1 holt das Bild immer in voller Projektgröße.
        Mlt::Filter f(profile, "qtblend");
        if (f.is_valid()) {
            f.set("rect", mlt_rect{0, 0, double(profile.width()), double(profile.height()), 1.0});
            f.set("distort", 1);
            cut.attach(f);
        }
    }
    if (kind == TrackKind::Audio) {
        applyVolume(profile, cut, c, a, len);
        applyPan(profile, cut, c, a, len);
    }
}

// Titel: transparentes Vollbild ("color" mit Alpha) + qtext-Filter (Qt: Schrift, Farbe, Umrandung, Box).
// Das Rechteck des Filters wird auf die Breite des Textblocks gesetzt und um dessen Mitte positioniert,
// dann richtet halign nur die Zeilen im Block aus (wie DaVinci) statt den Block an den Bildrand zu schieben.
// a/len: Ausschnitt für Keyframes (Position, Farbe, Größe).
Mlt::Producer* titleCut(Mlt::Profile& profile, const Clip& c, int a, int len)
{
    const TitleStyle& t = c.title;
    // Mit Unschärfe: durchsichtige Fläche in Textfarbe statt Schwarz, sonst mischt gblur (nicht vormultipliziertes
    // Alpha) die schwarzen Nachbarpixel ein und der Text wird dunkel und grau
    QByteArray base = "color:#00000000";
    if (const EffectInstance* fx = EffectRegistry::instance(c, "blur"); fx && fx->enabled)
        base = "color:#00" + t.color.name(QColor::HexRgb).mid(1).toLatin1();
    Mlt::Producer src(profile, base.constData());
    src.set("length", len);
    src.set("out", len - 1);
    Mlt::Producer* cut = src.cut(0, len - 1); // Cut hält eine Referenz auf src
    const double W = profile.width(), H = profile.height();

    // Größe animiert: qtext kann "size" nicht animieren -> in der größten Größe zeichnen (scharf)
    // und per qtblend um die Textmitte verkleinern
    const bool sizeAnim = Keys::animated(c, AnimParam::TitleSize);
    double size = sizeAnim ? 0.0 : t.size;
    if (sizeAnim) // Ease-Kurven bleiben zwischen den Keyframe-Werten -> größter Keyframe = größte Größe
        for (const Keyframe& k : c.keys.value(AnimParam::TitleSize)) size = std::max(size, k.value);
    size = std::max(1.0, size);

    QFont font(t.font);
    font.setPixelSize(std::max(1, int(std::lround(size))));
    font.setBold(t.bold);
    font.setItalic(t.italic);
    const QFontMetricsF fm(font);
    double w = 1;
    for (const QString& line : t.text.split('\n')) w = std::max(w, fm.horizontalAdvance(line));

    Mlt::Filter f(profile, "qtext");
    if (!f.is_valid()) return cut;
    f.set("argument", t.text.toUtf8().constData());
    // Rechteck als mlt_rect (Zahlenformat, siehe applyTransform)
    auto geometry = [&](double tt) {
        return mlt_rect{(W - w) / 2 + Keys::valueAt(c, AnimParam::TitlePosX, tt), -Keys::valueAt(c, AnimParam::TitlePosY, tt),
                        w, H, 1.0};
    };
    if (!animate(c, {AnimParam::TitlePosX, AnimParam::TitlePosY}, a, len,
                 [&](int pos, double tt) { f.anim_set("geometry", geometry(tt), pos, len); }))
        f.set("geometry", geometry(a));
    f.set("family", t.font.toUtf8().constData());
    f.set("size", size);
    f.set("weight", t.bold ? 700 : 400);
    f.set("style", t.italic ? "italic" : "normal");
    f.set("halign", t.align == 0 ? "left" : t.align == 2 ? "right" : "center");
    f.set("valign", "middle");
    // Farben als "#aarrggbb"; bgcolour hat sonst ein leichtes Grau als Standard
    auto mltColor = [](const QColor& col) {
        return mlt_color{uint8_t(col.red()), uint8_t(col.green()), uint8_t(col.blue()), uint8_t(col.alpha())};
    };
    if (!animate(c, {AnimParam::TitleColor}, a, len, [&](int pos, double tt) {
            f.anim_set("fgcolour", mltColor(Keys::toColor(Keys::valueAt(c, AnimParam::TitleColor, tt))), pos, len);
        }))
        f.set("fgcolour", t.color.name(QColor::HexArgb).toUtf8().constData());
    f.set("bgcolour", t.boxOn ? t.boxColor.name(QColor::HexArgb).toUtf8().constData() : "#00000000");
    f.set("pad", t.boxOn ? std::max(0.0, t.boxPad) : 0.0);
    f.set("olcolour", t.outlineColor.name(QColor::HexArgb).toUtf8().constData());
    f.set("outline", t.outlineOn ? std::max(0.0, t.outlineWidth) : 0.0);
    attachTo(*cut, f);

    if (sizeAnim) {
        // Maßstab s = Größe/größte Größe um die Textmitte; Produkt aus Größe und Position -> Frame für Frame.
        // qtblend (distort=1) holt das Bild außerdem immer in Projektgröße (Umrandung, siehe decorate).
        Mlt::Filter scale(profile, "qtblend");
        if (scale.is_valid()) {
            animate(c, {AnimParam::TitleSize, AnimParam::TitlePosX, AnimParam::TitlePosY}, a, len, [&](int pos, double tt) {
                const double k = std::max(0.001, Keys::valueAt(c, AnimParam::TitleSize, tt) / size);
                const double cx = W / 2 + Keys::valueAt(c, AnimParam::TitlePosX, tt);
                const double cy = H / 2 - Keys::valueAt(c, AnimParam::TitlePosY, tt);
                scale.anim_set("rect", mlt_rect{cx * (1 - k), cy * (1 - k), W * k, H * k, 1.0}, pos, len);
            }, true);
            scale.set("distort", 1);
            attachTo(*cut, scale);
        }
    }
    return cut;
}

// Fade-Griffe des Clips auf den Ausschnitt [from, to) (Timeline-Frames) anwenden. Keyframes nur in den
// Fade-Bereichen und an ihren Grenzen, damit lange Clips nicht tausende Keyframes bekommen.
void applyClipFades(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind, int from, int to)
{
    const int length = c.length();
    const int fi = std::min(c.fadeIn, length);
    const int fo = std::min(c.fadeOut, length - fi);
    const int a = from - c.start, b = to - c.start; // clip-lokal, b exklusiv
    const int len = b - a;
    if ((fi <= 0 || a >= fi) && (fo <= 0 || b <= length - fo)) return; // Ausschnitt berührt keinen Fade
    auto isKey = [&](int t) { return t == a || t == b - 1 || t <= fi || t >= length - fo - 1; };
    const bool video = kind == TrackKind::Video;
    Mlt::Filter f(profile, video ? "brightness" : "volume");
    if (!f.is_valid()) return;
    if (video) f.set("level", 1.0);
    for (int t = a; t < b; ++t) {
        if (!isKey(t)) continue;
        // dieselbe Kurve zeichnet die Timeline (Wellenform, Fade-Linie)
        if (video) {
            f.anim_set("alpha", TimelineOps::fadeRamp(c, t), t - a, len);
        } else {
            const double g = TimelineOps::audioFadeGain(c, t);
            f.anim_set("level", g > 0.001 ? 20.0 * std::log10(g) : -200.0, t - a, len);
        }
    }
    f.set_in_and_out(cut.get_in(), cut.get_out()); // Keyframes zählen ab Filter-In
    cut.attach(f);
}

// Video ein-/ausblenden über `len` Frames über den Alphakanal (auf V1 = aus Schwarz, darüber = zur
// Spur darunter, wie DaVinci). Keyframes per anim_set statt als Text: "0.5" würde sonst je nach LC_NUMERIC als 0 gelesen.
void applyFade(Mlt::Profile& profile, Mlt::Producer& cut, int len, bool fadeIn)
{
    auto value = [&](int i) { return fadeIn ? double(i) / len : double(len - 1 - i) / len; };
    Mlt::Filter f(profile, "brightness");
    if (!f.is_valid()) return;
    f.set("level", 1.0);
    f.anim_set("alpha", value(0), 0, len);
    f.anim_set("alpha", value(len - 1), len - 1, len);
    f.set_in_and_out(cut.get_in(), cut.get_out()); // Keyframes zählen ab Filter-In
    cut.attach(f);
}

// Audio-Übergang: Pegel von Clip clipId im Ausschnitt des Übergangs als Keyframe in dB pro Frame
// (Kurve aus TimelineOps, dieselbe zeichnet die Wellenform)
void applyAudioTransition(Mlt::Profile& profile, Mlt::Producer& cut, const TimelineOps::TransitionSpan& s, int clipId)
{
    Mlt::Filter f(profile, "volume");
    if (!f.is_valid()) return;
    const int len = s.length();
    for (int i = 0; i < len; ++i) {
        const double v = TimelineOps::audioTransitionGain(s, clipId, s.start + i);
        f.anim_set("level", v > 0.001 ? 20.0 * std::log10(v) : -200.0, i, len);
    }
    f.set_in_and_out(cut.get_in(), cut.get_out()); // Keyframes zählen ab Filter-In
    cut.attach(f);
}

// Deckkraft-Keyframes (brightness/alpha) über einen Ausschnitt der Länge len: (Frame, Wert)
void applyAlphaKeys(Mlt::Profile& profile, Mlt::Producer& cut, int len, std::initializer_list<std::pair<int, double>> keys)
{
    Mlt::Filter f(profile, "brightness");
    if (!f.is_valid()) return;
    f.set("level", 1.0);
    for (const auto& [pos, v] : keys) f.anim_set("alpha", v, std::clamp(pos, 0, len - 1), len);
    f.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(f);
}

// Verlaufsbild für luma (Graustufen-PNG, einmal erzeugt): dunkle Stellen wechseln zuerst.
// PNG statt PGM: luma lädt es über einen Bild-Producer und skaliert es sauber auf die Vorschaugröße.
QString wipeLuma(TransitionType type, int w, int h)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/lumas";
    const QString path = QString("%1/%2_%3x%4.png").arg(dir, transitionTypeInfo(type).id).arg(w).arg(h);
    if (QFile::exists(path)) return path;
    QDir().mkpath(dir);
    QImage img(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double t = 0;
            switch (type) {
            case TransitionType::WipeRight: t = double(x) / std::max(1, w - 1); break;
            case TransitionType::WipeLeft: t = 1.0 - double(x) / std::max(1, w - 1); break;
            case TransitionType::WipeDown: t = double(y) / std::max(1, h - 1); break;
            case TransitionType::WipeUp: t = 1.0 - double(y) / std::max(1, h - 1); break;
            default: break;
            }
            line[x] = uchar(std::lround(t * 255));
        }
    }
    // Erst unter anderem Namen schreiben, damit eine zweite Instanz nie ein halbes Bild liest
    const QString tmp = path + ".tmp.png";
    if (!img.save(tmp)) return {};
    QFile::remove(path);
    QFile::rename(tmp, path);
    return path;
}

// Rand einer Wischblende: Farbfläche, per animiertem qtcrop auf einen Streifen um die Kante begrenzt.
// edge0/edge1 = Lage der Kante (Anteil 0..1 in Wischrichtung) im ersten/letzten Frame, dazwischen linear.
void addWipeBorder(Mlt::Profile& profile, Mlt::Tractor& mix, int track, int len, const TransitionStyle& st,
                   double edge0, double edge1)
{
    QColor c = st.borderColor;
    c.setAlpha(255);
    Mlt::Producer strip(profile, ("color:" + c.name(QColor::HexArgb)).toUtf8().constData());
    if (!strip.is_valid()) return;
    strip.set("length", len);
    strip.set_in_and_out(0, len - 1);
    Mlt::Filter crop(profile, "qtcrop");
    if (!crop.is_valid()) return;
    const double W = profile.width(), H = profile.height();
    const bool horizontal = st.type == TransitionType::WipeRight || st.type == TransitionType::WipeLeft;
    // nach links/oben: Kante läuft vom Ende zum Anfang
    const bool reverse = st.type == TransitionType::WipeLeft || st.type == TransitionType::WipeUp;
    const double full = horizontal ? W : H;
    for (const auto& [i, e] : {std::pair{0, edge0}, std::pair{len - 1, edge1}}) {
        const double pos = (reverse ? 1.0 - e : e) * full;
        mlt_rect r{0, 0, W, H, 1.0};
        (horizontal ? r.x : r.y) = pos - st.border / 2;
        (horizontal ? r.w : r.h) = st.border;
        crop.anim_set("rect", r, i, len);
    }
    crop.set("color", "#00000000");
    crop.set_in_and_out(0, len - 1);
    strip.attach(crop);
    mix.set_track(strip, track);
    Mlt::Transition t(profile, "qtblend");
    t.set("always_active", 1);
    mix.plant_transition(t, 0, track);
}

// Video-Übergang außer Cross Dissolve als kleiner Tractor: a geht, b kommt; fehlt eine Seite, ist dort Leere.
std::unique_ptr<Mlt::Tractor> styledTransition(Mlt::Profile& profile, Mlt::Producer* a, Mlt::Producer* b, int len,
                                               const TransitionStyle& st)
{
    const TransitionType type = st.type;
    auto mix = std::make_unique<Mlt::Tractor>(profile);
    if (type == TransitionType::DipToColor) {
        // Farbfläche unten, a blendet in der ersten Hälfte aus, b in der zweiten ein (ohne Gegenseite: ganze Länge)
        QColor c = st.color;
        c.setAlpha(255); // "#aarrggbb" wie beim Titel
        Mlt::Producer color(profile, ("color:" + c.name(QColor::HexArgb)).toUtf8().constData());
        color.set("length", len);
        color.set_in_and_out(0, len - 1);
        mix->set_track(color, 0);
        const int half = a && b ? len / 2 : 0;
        int track = 1;
        for (Mlt::Producer* p : {a, b}) {
            if (!p) continue;
            if (p == a) applyAlphaKeys(profile, *p, len, {{0, 1.0}, {(b ? half : len) - 1, 0.0}, {len - 1, 0.0}});
            else applyAlphaKeys(profile, *p, len, {{0, 0.0}, {half, 0.0}, {len - 1, 1.0}});
            mix->set_track(*p, track);
            Mlt::Transition t(profile, "qtblend");
            t.set("always_active", 1);
            mix->plant_transition(t, 0, track);
            ++track;
        }
        return mix;
    }
    const bool border = st.border >= 0.5;
    if (!a || !b) {
        // Wischblende ins/aus dem Leeren: luma ignoriert Transparenz -> sichtbaren Bereich per qtcrop animieren
        // (harte Kante, Weichheit wirkt hier nicht)
        Mlt::Producer& one = a ? *a : *b;
        // Die neue Seite kommt aus der Startrichtung: nach rechts = von links usw.
        const bool fromStart = (type == TransitionType::WipeRight || type == TransitionType::WipeDown) == !a;
        const bool horizontal = type == TransitionType::WipeRight || type == TransitionType::WipeLeft;
        double edges[2] = {0, 0}; // Lage der Kante für den Rand (Anteil in Wischrichtung)
        Mlt::Filter crop(profile, "qtcrop");
        if (crop.is_valid()) {
            const double W = profile.width(), H = profile.height();
            for (int k = 0; k < 2; ++k) {
                const int i = k ? len - 1 : 0;
                // p = Anteil, den die neue Seite schon einnimmt (Einblenden: der Clip selbst, Ausblenden: die Leere)
                const double p = len > 1 ? double(i) / (len - 1) : 1.0;
                const double shown = a ? 1.0 - p : p; // sichtbarer Anteil des Clips
                mlt_rect r{0, 0, W, H, 1.0};
                double& pos = horizontal ? r.x : r.y;
                double& size = horizontal ? r.w : r.h;
                const double full = horizontal ? W : H;
                size = full * shown;
                pos = fromStart ? 0.0 : full - size;
                crop.anim_set("rect", r, i, len);
                edges[k] = p; // die Kante wandert in Wischrichtung von 0 nach 1
            }
            crop.set("color", "#00000000");
            crop.set_in_and_out(one.get_in(), one.get_out());
            one.attach(crop);
        }
        mix->set_track(one, 0);
        if (border) addWipeBorder(profile, *mix, 1, len, st, edges[0], edges[1]);
        return mix;
    }
    // Wischblende zwischen zwei Clips: luma mit Verlaufsbild
    mix->set_track(*a, 0);
    mix->set_track(*b, 1);
    Mlt::Transition t(profile, "luma");
    const QString luma = wipeLuma(type, profile.width(), profile.height());
    if (!luma.isEmpty()) t.set("resource", luma.toUtf8().constData());
    // Weichheit: Anteil des Verlaufs, über den überblendet wird (etwas Weichheit immer, gegen Treppchen)
    const double soft = 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0;
    t.set("softness", soft);
    t.set_in_and_out(0, len - 1);
    mix->plant_transition(t, 0, 1);
    if (border) {
        // luma: Fortschritt p = i/len, Übergangszone endet bei p*(1+soft) -> Mitte der Zone als Kante
        auto edge = [&](int i) { return double(i) / len * (1.0 + soft) - soft / 2; };
        addWipeBorder(profile, *mix, 2, len, st, edge(0), edge(len - 1));
    }
    return mix;
}

// Untertitel: Zeilen auf maxWidth umbrechen (vorhandene Zeilenumbrüche bleiben), zu lange Wörter bleiben ganz
QStringList wrapSubtitle(const QString& text, const QFontMetricsF& fm, double maxWidth)
{
    QStringList out;
    for (const QString& para : text.split('\n')) {
        QString line;
        for (const QString& word : para.split(' ', Qt::SkipEmptyParts)) {
            const QString candidate = line.isEmpty() ? word : line + ' ' + word;
            if (!line.isEmpty() && fm.horizontalAdvance(candidate) > maxWidth) {
                out << line;
                line = word;
            } else {
                line = candidate;
            }
        }
        out << line;
    }
    return out;
}

// Ein Untertitel (Spurstil wie ein Titel): Textblock unten, Unterkante posY Pixel über dem Bildrand,
// Zeilen auf 90 % der Bildbreite umbrochen. len Frames lang.
Mlt::Producer* subtitleCut(Mlt::Profile& profile, const TitleStyle& t, const QString& text, int len)
{
    Mlt::Producer src(profile, "color:#00000000");
    src.set("length", len);
    src.set("out", len - 1);
    Mlt::Producer* cut = src.cut(0, len - 1);
    const double W = profile.width(), H = profile.height();
    QFont font(t.font);
    font.setPixelSize(std::max(1, int(std::lround(t.size))));
    font.setBold(t.bold);
    font.setItalic(t.italic);
    const QFontMetricsF fm(font);
    const QStringList lines = wrapSubtitle(text.trimmed(), fm, W * 0.9);
    double w = 1;
    for (const QString& line : lines) w = std::max(w, fm.horizontalAdvance(line));
    Mlt::Filter f(profile, "qtext");
    if (!f.is_valid()) return cut;
    f.set("argument", lines.join('\n').toUtf8().constData());
    const double bottom = std::clamp(H - t.posY, 1.0, H);
    f.set("geometry", mlt_rect{(W - w) / 2 + t.posX, 0, w, bottom, 1.0});
    f.set("family", t.font.toUtf8().constData());
    f.set("size", std::max(1.0, t.size));
    f.set("weight", t.bold ? 700 : 400);
    f.set("style", t.italic ? "italic" : "normal");
    f.set("halign", t.align == 0 ? "left" : t.align == 2 ? "right" : "center");
    f.set("valign", "bottom");
    f.set("fgcolour", t.color.name(QColor::HexArgb).toUtf8().constData());
    f.set("bgcolour", t.boxOn ? t.boxColor.name(QColor::HexArgb).toUtf8().constData() : "#00000000");
    f.set("pad", t.boxOn ? std::max(0.0, t.boxPad) : 0.0);
    f.set("olcolour", t.outlineColor.name(QColor::HexArgb).toUtf8().constData());
    f.set("outline", t.outlineOn ? std::max(0.0, t.outlineWidth) : 0.0);
    attachTo(*cut, f);
    // wie bei Titeln: immer in voller Projektgröße zeichnen (Umrandung skaliert sonst in der Vorschau nicht mit)
    Mlt::Filter full(profile, "qtblend");
    if (full.is_valid()) {
        full.set("rect", mlt_rect{0, 0, W, H, 1.0});
        full.set("distort", 1);
        attachTo(*cut, full);
    }
    return cut;
}

} // namespace

TimelineBuilder::TimelineBuilder(Mlt::Profile& profile)
    : m_profile(profile), m_factory(std::make_unique<ProducerFactory>(profile))
{
}
TimelineBuilder::~TimelineBuilder() = default;

Mlt::Producer* TimelineBuilder::producerFor(const QString& path, TrackKind kind, int trackIndex, bool second,
                                            const Clip* retime)
{
    // Proxy (nur Vorschau): Schlüssel mit der tatsächlich gelesenen Datei, damit Umschalten neu öffnet
    const QString file = m_resolver ? m_resolver(path, kind) : path;
    // Geschwindigkeit: MLT timewarp (negativ = rückwärts); Standbild: wiederholtes Einzelframe (s. u.)
    const bool freeze = retime && retime->freeze;
    const double warp = retime && !freeze ? (retime->reverse ? -retime->speed : retime->speed) : 1.0;
    const bool pitch = retime && retime->keepPitch;
    QString resource = file;
    QString tag;
    if (warp != 1.0) {
        // timewarp liest die Zahl per atof -> im gerade gültigen C-Locale schreiben (Komma/Punkt, siehe dev-notes)
        char num[32];
        std::snprintf(num, sizeof num, "%.10g", warp);
        resource = QString("timewarp:%1:%2").arg(QString::fromLatin1(num), file);
        tag = QString("|w%1%2").arg(warp).arg(pitch ? "p" : "");
    }
    if (freeze) tag = QString("|f%1|%2%3").arg(retime->in).arg(retime->speed).arg(retime->reverse ? "r" : "");
    const QString key = m_keyPrefix + QString("%1%2|%3|%4%5").arg(kind == TrackKind::Video ? "v" : "a").arg(second ? "x" : "")
                            .arg(trackIndex).arg(file, tag);
    m_used.insert(key);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();

    // Video über die Fabrik (BT.601-Quellen, siehe Profiles.h); Ton braucht das nicht
    auto p = kind == TrackKind::Video ? m_factory->open(resource)
                                      : std::make_unique<Mlt::Producer>(m_profile, resource.toUtf8().constData());
    if (!p->is_valid()) return nullptr;
    if (warp != 1.0 && kind == TrackKind::Audio) p->set("warp_pitch", pitch ? 1 : 0);
    // Nicht benötigten Stream gar nicht erst dekodieren
    if (kind == TrackKind::Video) p->set("audio_index", -1);
    else p->set("video_index", -1);
    // Standbilder lassen sich beliebig lang ziehen (sonst begrenzt MLT auf die Standardlänge)
    if (const QByteArray svc = p->get("mlt_service"); svc == "qimage" || svc == "pixbuf") {
        const int len = 24 * 3600 * qRound(m_profile.fps());
        p->set("length", len);
        p->set("out", len - 1);
    }
    if (freeze) {
        // Standbild: Frame `in` (in Datei-Frames umgerechnet) als 1-Frame-Ausschnitt, in einer Playlist
        // beliebig oft wiederholt. (MLT hold skaliert im Tractor falsch, der kdenlive-Filter freeze stürzte ab.)
        const int fileLen = p->get_length();
        const double at = retime->in * retime->speed;
        const int frame = std::clamp(int(retime->reverse ? fileLen - 1 - at : at), 0, std::max(0, fileLen - 1));
        auto hold = std::make_unique<Mlt::Playlist>(m_profile);
        std::unique_ptr<Mlt::Producer> one(p->cut(frame, frame)); // Cut hält eine Referenz auf p
        hold->append(*one);
        hold->repeat(0, 24 * 3600 * qRound(m_profile.fps()));
        // Für sourceAspect (Transform seitenverhältnis-treu)
        const int vi = p->get_int("video_index");
        const QByteArray rotate = QString("meta.media.%1.codec.rotate").arg(vi).toUtf8();
        for (const char* name : {"width", "height", "aspect_ratio", "video_index", "meta.media.width",
                                 "meta.media.height", rotate.constData()})
            if (const char* v = p->get(name)) hold->set(name, v);
        p = std::move(hold);
    }
    Mlt::Producer* raw = p.get();
    m_cache.emplace(key, std::move(p));
    return raw;
}

Mlt::Producer* TimelineBuilder::clipAudioSource(const Clip& c)
{
    if (c.isTitle() || c.freeze || c.mediaPath.isEmpty()) return nullptr;
    m_used.clear(); // nur dieser eine Producer bleibt im Cache
    Mlt::Producer* p = producerFor(c.mediaPath, TrackKind::Audio, 0, false, &c);
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = m_used.count(it->first) ? std::next(it) : m_cache.erase(it);
    return p;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::buildClipOutput(const Clip& clip)
{
    Clip c = clip;
    c.start = 0;
    c.enabled = true;
    c.fadeIn = c.fadeOut = 0;
    c.transIn = c.transOut = 0;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    tl.masterLimiter = false;
    m_transparent = true;
    auto tractor = build(tl);
    m_transparent = false;
    return tractor;
}

bool TimelineBuilder::applyMixer(const Timeline& tl, const MixerHooks& hooks)
{
    if (int(hooks.tracks.size()) != tl.audio.size() || !hooks.master.volume) return false;
    for (int i = 0; i < tl.audio.size(); ++i) {
        const MixerHooks::Strip& h = hooks.tracks[i];
        if (!h.volume || !h.pan) return false;
        h.volume->set("level", mltLevel(tl.audio[i].volumeDb));
        h.pan->set("start", mltPan(tl.audio[i].pan));
    }
    hooks.master.volume->set("level", mltLevel(tl.masterVolumeDb));
    if (hooks.master.limiter) setLimiter(*hooks.master.limiter, tl.masterLimiter, tl.masterLimiterDb);
    return true;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::build(const Timeline& tl, MixerHooks* hooks)
{
    if (hooks) *hooks = {};
    m_used.clear();
    m_nestedCache.clear();
    m_nested = tl.nested;
    struct BypassScope { // Filter der Farbkorrektur bekommen den Schalter dieses Builders
        explicit BypassScope(std::shared_ptr<std::atomic<bool>> f) { t_gradeBypass = std::move(f); }
        ~BypassScope() { t_gradeBypass.reset(); }
    } bypassScope(m_gradeBypass);
    auto tractor = buildTimeline(tl, hooks);
    m_nested.reset();
    m_nestedCache.clear(); // Cuts halten ihre verschachtelten Tractoren selbst (MLT-Referenzzählung)
    // Nicht mehr benutzte Producer schließen (z. B. Original nach Umschalten auf den Proxy oder gelöschter Clip);
    // Cuts im alten Tractor halten ihre Quelle per MLT-Referenzzählung selbst am Leben
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = m_used.count(it->first) ? std::next(it) : m_cache.erase(it);
    return tractor;
}

Mlt::Producer* TimelineBuilder::nestedProducer(int sequenceId, TrackKind kind, int trackIndex, bool second)
{
    if (!m_nested || m_nestStack.size() >= 16 || m_nestStack.count(sequenceId)) return nullptr; // Schleife/zu tief
    const auto it = m_nested->constFind(sequenceId);
    if (it == m_nested->cend()) return nullptr;
    // Wie bei Dateien: je Spur und Seite eines Übergangs ein eigener Tractor (eigene Decoder)
    const QString key = QString("%1|%2|%3|%4|%5").arg(sequenceId).arg(kind == TrackKind::Video ? "v" : "a")
                            .arg(trackIndex).arg(second ? "x" : "").arg(m_nestStack.size());
    if (auto c = m_nestedCache.find(key); c != m_nestedCache.end()) return c->second.get();
    m_nestStack.insert(sequenceId);
    const bool transparent = std::exchange(m_transparent, true); // leere Stellen zeigen die Spur darunter
    const bool subtitles = std::exchange(m_subtitles, false);    // Untertitel gehören zur äußeren Timeline
    const bool nested = std::exchange(m_inNested, true);
    // Producer im Inneren getrennt von denen der äußeren Timeline (eigene Decoder, siehe producerFor)
    const QString prefix = std::exchange(m_keyPrefix, m_keyPrefix + "n" + key + "/");
    std::unique_ptr<Mlt::Producer> p = buildTimeline(*it, nullptr);
    m_keyPrefix = prefix;
    m_inNested = nested;
    m_subtitles = subtitles;
    m_transparent = transparent;
    m_nestStack.erase(sequenceId);
    if (!p || !p->is_valid()) return nullptr;
    Mlt::Producer* raw = p.get();
    m_nestedCache.emplace(key, std::move(p));
    return raw;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::buildTimeline(const Timeline& tl, MixerHooks* hooks)
{
    // Solo wie DaVinci: sobald eine Spur Solo hat, sind alle anderen Audiospuren stumm
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    auto tractor = std::make_unique<Mlt::Tractor>(m_profile);
    // Verschachtelt: Hintergrund weit über das Ende hinaus, damit ein Compound Clip nach Kürzen seines Inhalts
    // (Clip länger als die Sequenz) leere Frames statt Wiederholungen liefert
    const int content = std::max(1, TimelineOps::endFrame(tl));
    const int end = m_inNested ? content + 3600 * qRound(m_profile.fps()) : content;

    // Spur 0: schwarzer Hintergrund über die ganze Länge
    Mlt::Playlist background(m_profile);
    Mlt::Producer black(m_profile, m_transparent ? "color:#00000000" : "color:black");
    black.set("length", end);
    background.append(black, 0, end - 1);
    tractor->set_track(background, 0);

    int mltIndex = 1;
    auto fill = [&](const Track& track, TrackKind kind, int trackIndex) {
        Mlt::Playlist pl(m_profile);
        auto isStill = [](Mlt::Producer* p) {
            const QByteArray svc = p->get("mlt_service");
            return svc == "qimage" || svc == "pixbuf";
        };
        const TimelineOps::SourceLength srcLen = [&](const Clip& c) {
            if (c.isCompound()) {
                if (!m_nested) return 0;
                const auto it = m_nested->constFind(c.sequenceId);
                return it == m_nested->cend() ? 0 : TimelineOps::endFrame(*it);
            }
            Mlt::Producer* p = c.mediaPath.isEmpty() ? nullptr : producerFor(c.mediaPath, kind, trackIndex); // leer = Titel
            return p && !isStill(p) ? c.retimedLength(p->get_length()) : 0;
        };
        const QVector<TimelineOps::TransitionSpan> spans = TimelineOps::transitions(track, srcLen);
        QHash<int, const Clip*> byId;
        for (const Clip& c : track.clips) byId.insert(c.id, &c);

        // Ausschnitt [from, to) der Timeline aus Clip c (darf über In/Out hinaus in die Handles reichen)
        auto cutOf = [&](const Clip& c, int from, int to, bool second) -> Mlt::Producer* {
            if (c.isTitle()) {
                if (!c.enabled || kind != TrackKind::Video) return nullptr;
                Mlt::Producer* cut = titleCut(m_profile, c, from - c.start, to - from);
                decorate(m_profile, *cut, c, kind, from - c.start, to - from);
                applyClipFades(m_profile, *cut, c, kind, from, to);
                return cut;
            }
            if (c.isCompound()) { // Inhalt der Sequenz als eigener Tractor (Frames zählen ab Sequenzanfang)
                Mlt::Producer* src = c.enabled ? nestedProducer(c.sequenceId, kind, trackIndex, second) : nullptr;
                const int in = c.in + (from - c.start);
                if (!src || in < 0) return nullptr;
                Mlt::Producer* cut = src->cut(in, in + (to - from) - 1);
                decorate(m_profile, *cut, c, kind, from - c.start, to - from);
                applyClipFades(m_profile, *cut, c, kind, from, to);
                return cut;
            }
            if (c.freeze && kind == TrackKind::Audio) return nullptr; // Standbild ist stumm (wie DaVinci)
            // Render-Cache (nur Vorschau): fertige Clip-Ausgabe statt Original + Effekte; Frame 0 = Clip-Anfang.
            // Nur innerhalb des Clips (Übergänge mit Handles davor/danach kommen weiter aus dem Original).
            // Bei Vorher/Nachher enthält der Cache die Farbkorrektur -> solche Clips dann aus dem Original.
            const bool gradeBypassed = m_gradeBypass && m_gradeBypass->load()
                && std::any_of(c.effects.begin(), c.effects.end(),
                               [](const EffectInstance& e) { return e.enabled && e.effectId == QLatin1String("grade"); });
            if (kind == TrackKind::Video && m_clipCache && c.enabled && !gradeBypassed && from >= c.start && to <= c.end()) {
                if (const QString file = m_clipCache(c); !file.isEmpty()) {
                    if (Mlt::Producer* cache = producerFor(file, kind, trackIndex, second)) {
                        Mlt::Producer* cut = cache->cut(from - c.start, to - c.start - 1);
                        applyClipFades(m_profile, *cut, c, kind, from, to); // Fades sind nicht im Cache
                        return cut;
                    }
                }
            }
            Mlt::Producer* src = c.enabled ? producerFor(c.mediaPath, kind, trackIndex, second, &c) : nullptr;
            if (!src) return nullptr;
            int in = c.in + (from - c.start);
            if (in < 0 && isStill(src)) in = 0; // Standbild: jedes Frame gleich
            if (in < 0) return nullptr;
            Mlt::Producer* cut = src->cut(in, in + (to - from) - 1);
            decorate(m_profile, *cut, c, kind, from - c.start, to - from);
            applyClipFades(m_profile, *cut, c, kind, from, to);
            return cut;
        };

        int cursor = 0;
        auto blankTo = [&](int frame) {
            if (frame > cursor) pl.blank(frame - cursor - 1);
            cursor = std::max(cursor, frame);
        };
        auto appendSpan = [&](const TimelineOps::TransitionSpan& s) {
            blankTo(s.start);
            const Clip* a = byId.value(s.leftId);
            const Clip* b = byId.value(s.rightId);
            std::unique_ptr<Mlt::Producer> ca(a ? cutOf(*a, s.start, s.end, false) : nullptr);
            std::unique_ptr<Mlt::Producer> cb(b ? cutOf(*b, s.start, s.end, true) : nullptr);
            const int len = s.length();
            if (kind == TrackKind::Video && s.style.type != TransitionType::CrossDissolve && (ca || cb)) {
                pl.append(*styledTransition(m_profile, ca.get(), cb.get(), len, s.style));
            } else if (ca && cb) {
                // Cross Dissolve: beide Seiten in einem kleinen Tractor, Überblendung von a nach b
                Mlt::Tractor mix(m_profile);
                mix.set_track(*ca, 0);
                mix.set_track(*cb, 1);
                Mlt::Transition t(m_profile, kind == TrackKind::Video ? "luma" : "mix");
                if (kind == TrackKind::Audio) {
                    // Crossfade: beide Seiten mit eigener Kurve, dann einfach addieren
                    applyAudioTransition(m_profile, *ca, s, a->id);
                    applyAudioTransition(m_profile, *cb, s, b->id);
                    t.set("start", 1.0);
                    t.set("sum", 1);
                }
                t.set_in_and_out(0, len - 1);
                mix.plant_transition(t, 0, 1);
                pl.append(mix);
            } else if (ca || cb) {
                // Nur eine Seite (Schnitt zum Leeren oder anderer Clip deaktiviert/offline): Aus-/Einblenden
                Mlt::Producer& one = ca ? *ca : *cb;
                if (kind == TrackKind::Video) applyFade(m_profile, one, len, !ca);
                else applyAudioTransition(m_profile, one, s, ca ? a->id : b->id);
                pl.append(one);
            } else {
                pl.blank(len - 1);
            }
            cursor = s.end;
        };

        for (const Clip& c : track.clips) {
            int bodyStart = c.start, bodyEnd = c.end();
            for (const auto& s : spans) {
                if (s.rightId == c.id) bodyStart = std::max(bodyStart, s.end);
                if (s.leftId == c.id) bodyEnd = std::min(bodyEnd, s.start);
            }
            // Einblenden (Übergang, der in diesem Clip beginnt und keinen linken Clip hat)
            for (const auto& s : spans)
                if (s.rightId == c.id && !s.leftId) appendSpan(s);
            if (bodyEnd > bodyStart) {
                blankTo(bodyStart);
                if (std::unique_ptr<Mlt::Producer> body(cutOf(c, bodyStart, bodyEnd, false)); body) pl.append(*body);
                else pl.blank(bodyEnd - bodyStart - 1); // deaktiviert oder Datei fehlt -> Lücke
                cursor = bodyEnd;
            }
            // Ausblenden bzw. Überblendung zum nächsten Clip
            for (const auto& s : spans)
                if (s.leftId == c.id) appendSpan(s);
        }
        // hide: 1 = kein Bild, 2 = kein Ton
        int hide = (kind == TrackKind::Video) ? 2 : 1;
        if (kind == TrackKind::Video && track.hidden) hide |= 1;
        if (kind == TrackKind::Audio && (track.muted || (anySolo && !track.solo))) hide |= 2;
        pl.set("hide", hide);
        if (kind == TrackKind::Audio) {
            MixerHooks::Strip* h = nullptr;
            if (hooks) {
                h = &hooks->tracks.emplace_back();
                h->audible = !(hide & 2);
            }
            attachStrip(m_profile, pl, track.volumeDb, track.pan, h);
        }
        tractor->set_track(pl, mltIndex);

        if (kind == TrackKind::Video) {
            Mlt::Transition t(m_profile, "qtblend");
            t.set("always_active", 1);
            tractor->plant_transition(t, 0, mltIndex);
        } else {
            Mlt::Transition t(m_profile, "mix");
            t.set("always_active", 1);
            t.set("sum", 1);
            tractor->plant_transition(t, 0, mltIndex);
        }
        ++mltIndex;
    };

    for (int i = 0; i < tl.video.size(); ++i) fill(tl.video[i], TrackKind::Video, i);
    // Untertitel über allen Videospuren (sichtbare Spur; Export nur beim Einbrennen)
    if (m_subtitles)
        for (const SubtitleTrack& st : tl.subtitles) {
            if (!st.enabled || st.cues.isEmpty()) continue;
            Mlt::Playlist pl(m_profile);
            int cursor = 0;
            for (const SubtitleCue& c : st.cues) {
                if (c.end <= cursor || c.text.trimmed().isEmpty()) continue;
                const int from = std::max(cursor, c.start);
                if (from > cursor) pl.blank(from - cursor - 1);
                std::unique_ptr<Mlt::Producer> cut(subtitleCut(m_profile, st.style, c.text, c.end - from));
                pl.append(*cut);
                cursor = c.end;
            }
            pl.set("hide", 2); // kein Ton
            tractor->set_track(pl, mltIndex);
            Mlt::Transition t(m_profile, "qtblend");
            t.set("always_active", 1);
            tractor->plant_transition(t, 0, mltIndex);
            ++mltIndex;
        }
    for (int i = 0; i < tl.audio.size(); ++i) fill(tl.audio[i], TrackKind::Audio, i);

    // Master-Fader auf dem Tractor (gilt damit auch für den Export)
    attachStrip(m_profile, *tractor, tl.masterVolumeDb, 0.0, hooks ? &hooks->master : nullptr,
                std::make_pair(tl.masterLimiter, tl.masterLimiterDb));
    return tractor;
}
