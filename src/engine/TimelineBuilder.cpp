#include "engine/TimelineBuilder.h"

#include "core/EffectRegistry.h"
#include "core/TimelineOps.h"

#include <Mlt.h>
#include <QColor>
#include <QString>
#include <algorithm>

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
    const double w = W * t.zoomX, h = H * t.zoomY;
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

} // namespace

TimelineBuilder::TimelineBuilder(Mlt::Profile& profile) : m_profile(profile) {}
TimelineBuilder::~TimelineBuilder() = default;

Mlt::Producer* TimelineBuilder::producerFor(const QString& path, TrackKind kind, int trackIndex)
{
    const QString key = QString("%1|%2|%3").arg(kind == TrackKind::Video ? "v" : "a").arg(trackIndex).arg(path);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) return it->second.get();

    auto p = std::make_unique<Mlt::Producer>(m_profile, path.toUtf8().constData());
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
        int cursor = 0;
        for (const Clip& c : track.clips) {
            if (c.start > cursor) pl.blank(c.start - cursor - 1);
            Mlt::Producer* src = c.enabled ? producerFor(c.mediaPath, kind, trackIndex) : nullptr;
            if (src) {
                pl.append(*src, c.in, c.out);
                std::unique_ptr<Mlt::Producer> cut(pl.get_clip(pl.count() - 1));
                if (cut) {
                    applyEffects(m_profile, *cut, c);
                    if (kind == TrackKind::Video) applyTransform(m_profile, *cut, c);
                    if (kind == TrackKind::Audio) {
                        applyVolume(m_profile, *cut, c);
                        applyPan(m_profile, *cut, c);
                    }
                }
            } else {
                pl.blank(c.length() - 1); // deaktiviert oder Datei fehlt -> Lücke
            }
            cursor = c.end();
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
    return tractor;
}
