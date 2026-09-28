#include "engine/TimelineBuilder.h"

#include "core/EffectRegistry.h"
#include "core/TimelineOps.h"

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
#include <cmath>

namespace {

void applyEffects(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c)
{
    for (const auto& inst : c.effects) {
        if (!inst.enabled) continue;
        const EffectDescriptor* d = EffectRegistry::find(inst.effectId);
        if (!d) continue;
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

void applyVolume(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c)
{
    if (c.volumeDb == 0.0) return;
    Mlt::Filter f(profile, "volume");
    if (!f.is_valid()) return;
    f.set("level", c.volumeDb <= kMinVolumeDb ? -200.0 : c.volumeDb); // dB
    clip.attach(f);
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

void applyTransform(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c)
{
    const ClipTransform& t = c.transform;
    if (t.isIdentity()) return;
    const double W = profile.width(), H = profile.height();
    if (t.hasCrop()) {
        // Beschneiden = Rand transparent machen, Bild bleibt an seinem Platz (wie DaVinci)
        Mlt::Filter crop(profile, "qtcrop");
        if (crop.is_valid()) {
            const double w = std::max(1.0, W - t.cropLeft - t.cropRight);
            const double h = std::max(1.0, H - t.cropTop - t.cropBottom);
            crop.set("rect", mlt_rect{t.cropLeft, t.cropTop, w, h, 1.0});
            crop.set("color", "#00000000");
            clip.attach(crop);
        }
    }
    if (t.hasOpacity()) {
        // Deckkraft über den Alphakanal (wie Shotcut); die qtblend-Opacity lieferte nur Schwarz
        Mlt::Filter op(profile, "brightness");
        if (op.is_valid()) {
            op.set("level", 1.0);
            op.set("alpha", t.opacity / 100.0);
            clip.attach(op);
        }
    }
    if (!t.hasTransform()) return;
    Mlt::Filter f(profile, "qtblend");
    if (!f.is_valid()) return;
    // distort=1 (unten) zieht die Quelle aufs Rechteck -> Rechteck selbst seitenverhältnis-treu ins Bild einpassen
    // (Hochformat-Clip im Querformat-Projekt bekommt seitlich Platz, wie ohne Transform und wie in DaVinci)
    double fitW = W, fitH = H;
    if (const double a = c.isTitle() ? 0.0 : sourceAspect(clip); a > 0) {
        if (a > W / H) fitH = W / a;
        else fitW = H * a;
    }
    const double w = fitW * t.zoomX, h = fitH * t.zoomY;
    const double x = (W - w) / 2 + t.posX, y = (H - h) / 2 - t.posY;
    // Als mlt_rect statt Text setzen: MLT liest Text-Rechtecke mit dem System-Zahlenformat
    // (deutsch: "940.8" -> 940 und ".8" rutscht ins nächste Feld). 5. Wert (Deckkraft) muss 1 sein.
    f.set("rect", mlt_rect{x, y, w, h, 1.0});
    f.set("rotation", -t.rotation); // DaVinci: positiv = gegen den Uhrzeigersinn
    f.set("rotate_center", 1);
    // distort=1: Quelle in voller Größe holen und erst beim Zeichnen skalieren. Sonst fordert qtblend das Bild
    // in Zielhöhe an (bei kleinem Zoom nur ein paar Pixel -> Bildsalat) und hält X/Y-Zoom im Seitenverhältnis.
    f.set("distort", 1);
    clip.attach(f);
}

void applyPan(Mlt::Profile& profile, Mlt::Service& clip, const Clip& c)
{
    if (c.pan == 0.0) return;
    Mlt::Filter f(profile, "panner");
    if (!f.is_valid()) return;
    f.set("channel", -1); // Balance (Stereo)
    // "start" statt "split": split wirkt in MLT 7 unabhängig vom Wert wie ein fester Versatz (getestet)
    f.set("start", (c.pan + 100.0) / 200.0);
    clip.attach(f);
}

// Mixer: Spur-/Master-Fader und -Pan als Filter auf Playlist bzw. Tractor.
// Mit hooks werden die Filter immer angehängt (neutral bei 0 dB/Mitte), damit sie live verstellbar sind.
double mltLevel(double db) { return db <= kMinVolumeDb ? -200.0 : db; }
double mltPan(double pan) { return (pan + 100.0) / 200.0; }

void attachStrip(Mlt::Profile& profile, Mlt::Service& s, double db, double pan, MixerHooks::Strip* hook)
{
    if (!hook) { // Export: nur was nötig ist
        Clip c; // gleiche Umrechnung wie bei Clips
        c.volumeDb = db;
        c.pan = pan;
        applyVolume(profile, s, c);
        applyPan(profile, s, c);
        return;
    }
    hook->volume = std::make_shared<Mlt::Filter>(profile, "volume");
    hook->volume->set("level", mltLevel(db));
    s.attach(*hook->volume);
    hook->pan = std::make_shared<Mlt::Filter>(profile, "panner");
    hook->pan->set("channel", -1);
    hook->pan->set("start", mltPan(pan));
    s.attach(*hook->pan);
    hook->meter = std::make_shared<Mlt::Filter>(profile, "audiolevel");
    hook->meter->set("iec_scale", 0);
    hook->meter->set("dbpeak", 1); // _audio_level.N = Spitzenpegel in dBFS
    s.attach(*hook->meter);
}

void decorate(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind)
{
    applyEffects(profile, cut, c);
    if (kind == TrackKind::Video) applyTransform(profile, cut, c);
    if (c.isTitle() && !c.transform.hasTransform()) {
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
        applyVolume(profile, cut, c);
        applyPan(profile, cut, c);
    }
}

// Titel: transparentes Vollbild ("color" mit Alpha) + qtext-Filter (Qt: Schrift, Farbe, Umrandung, Box).
// Das Rechteck des Filters wird auf die Breite des Textblocks gesetzt und um dessen Mitte positioniert,
// dann richtet halign nur die Zeilen im Block aus (wie DaVinci) statt den Block an den Bildrand zu schieben.
Mlt::Producer* titleCut(Mlt::Profile& profile, const TitleStyle& t, int len)
{
    Mlt::Producer src(profile, "color:#00000000");
    src.set("length", len);
    src.set("out", len - 1);
    Mlt::Producer* cut = src.cut(0, len - 1); // Cut hält eine Referenz auf src
    const double W = profile.width(), H = profile.height();

    QFont font(t.font);
    font.setPixelSize(std::max(1, int(std::lround(t.size))));
    font.setBold(t.bold);
    font.setItalic(t.italic);
    const QFontMetricsF fm(font);
    double w = 1;
    for (const QString& line : t.text.split('\n')) w = std::max(w, fm.horizontalAdvance(line));

    Mlt::Filter f(profile, "qtext");
    if (!f.is_valid()) return cut;
    f.set("argument", t.text.toUtf8().constData());
    // Rechteck als mlt_rect (Zahlenformat, siehe applyTransform)
    f.set("geometry", mlt_rect{(W - w) / 2 + t.posX, -t.posY, w, H, 1.0});
    f.set("family", t.font.toUtf8().constData());
    f.set("size", std::max(1.0, t.size));
    f.set("weight", t.bold ? 700 : 400);
    f.set("style", t.italic ? "italic" : "normal");
    f.set("halign", t.align == 0 ? "left" : t.align == 2 ? "right" : "center");
    f.set("valign", "middle");
    // Farben als "#aarrggbb"; bgcolour hat sonst ein leichtes Grau als Standard
    f.set("fgcolour", t.color.name(QColor::HexArgb).toUtf8().constData());
    f.set("bgcolour", t.boxOn ? t.boxColor.name(QColor::HexArgb).toUtf8().constData() : "#00000000");
    f.set("pad", t.boxOn ? std::max(0.0, t.boxPad) : 0.0);
    f.set("olcolour", t.outlineColor.name(QColor::HexArgb).toUtf8().constData());
    f.set("outline", t.outlineOn ? std::max(0.0, t.outlineWidth) : 0.0);
    cut->attach(f);
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

} // namespace

TimelineBuilder::TimelineBuilder(Mlt::Profile& profile) : m_profile(profile) {}
TimelineBuilder::~TimelineBuilder() = default;

Mlt::Producer* TimelineBuilder::producerFor(const QString& path, TrackKind kind, int trackIndex, bool second)
{
    // Proxy (nur Vorschau): Schlüssel mit der tatsächlich gelesenen Datei, damit Umschalten neu öffnet
    const QString file = m_resolver ? m_resolver(path, kind) : path;
    const QString key = QString("%1%2|%3|%4").arg(kind == TrackKind::Video ? "v" : "a").arg(second ? "x" : "")
                            .arg(trackIndex).arg(file);
    m_used.insert(key);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();

    auto p = std::make_unique<Mlt::Producer>(m_profile, file.toUtf8().constData());
    if (!p->is_valid()) return nullptr;
    // Nicht benötigten Stream gar nicht erst dekodieren
    if (kind == TrackKind::Video) p->set("audio_index", -1);
    else p->set("video_index", -1);
    // Standbilder lassen sich beliebig lang ziehen (sonst begrenzt MLT auf die Standardlänge)
    if (const QByteArray svc = p->get("mlt_service"); svc == "qimage" || svc == "pixbuf") {
        const int len = 24 * 3600 * qRound(m_profile.fps());
        p->set("length", len);
        p->set("out", len - 1);
    }
    Mlt::Producer* raw = p.get();
    m_cache.emplace(key, std::move(p));
    return raw;
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
    return true;
}

std::unique_ptr<Mlt::Tractor> TimelineBuilder::build(const Timeline& tl, MixerHooks* hooks)
{
    // Solo wie DaVinci: sobald eine Spur Solo hat, sind alle anderen Audiospuren stumm
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    if (hooks) *hooks = {};
    m_used.clear();
    auto tractor = std::make_unique<Mlt::Tractor>(m_profile);
    const int end = std::max(1, TimelineOps::endFrame(tl));

    // Spur 0: schwarzer Hintergrund über die ganze Länge
    Mlt::Playlist background(m_profile);
    Mlt::Producer black(m_profile, "color:black");
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
        const TimelineOps::SourceLength srcLen = [&](const QString& path) {
            Mlt::Producer* p = path.isEmpty() ? nullptr : producerFor(path, kind, trackIndex); // leer = Titel
            return p && !isStill(p) ? p->get_length() : 0;
        };
        const QVector<TimelineOps::TransitionSpan> spans = TimelineOps::transitions(track, srcLen);
        QHash<int, const Clip*> byId;
        for (const Clip& c : track.clips) byId.insert(c.id, &c);

        // Ausschnitt [from, to) der Timeline aus Clip c (darf über In/Out hinaus in die Handles reichen)
        auto cutOf = [&](const Clip& c, int from, int to, bool second) -> Mlt::Producer* {
            if (c.isTitle()) {
                if (!c.enabled || kind != TrackKind::Video) return nullptr;
                Mlt::Producer* cut = titleCut(m_profile, c.title, to - from);
                decorate(m_profile, *cut, c, kind);
                applyClipFades(m_profile, *cut, c, kind, from, to);
                return cut;
            }
            Mlt::Producer* src = c.enabled ? producerFor(c.mediaPath, kind, trackIndex, second) : nullptr;
            if (!src) return nullptr;
            int in = c.in + (from - c.start);
            if (in < 0 && isStill(src)) in = 0; // Standbild: jedes Frame gleich
            if (in < 0) return nullptr;
            Mlt::Producer* cut = src->cut(in, in + (to - from) - 1);
            decorate(m_profile, *cut, c, kind);
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
    for (int i = 0; i < tl.audio.size(); ++i) fill(tl.audio[i], TrackKind::Audio, i);

    // Master-Fader auf dem Tractor (gilt damit auch für den Export)
    attachStrip(m_profile, *tractor, tl.masterVolumeDb, 0.0, hooks ? &hooks->master : nullptr);
    // Nicht mehr benutzte Producer schließen (z. B. Original nach Umschalten auf den Proxy oder gelöschter Clip);
    // Cuts im alten Tractor halten ihre Quelle per MLT-Referenzzählung selbst am Leben
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = m_used.count(it->first) ? std::next(it) : m_cache.erase(it);
    return tractor;
}
