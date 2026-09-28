#include "engine/TimelineBuilder.h"

#include "core/EffectRegistry.h"
#include "core/TimelineOps.h"

#include <Mlt.h>
#include <QColor>
#include <QHash>
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

void applyVolume(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c)
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

void applyPan(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c)
{
    if (c.pan == 0.0) return;
    Mlt::Filter f(profile, "panner");
    if (!f.is_valid()) return;
    f.set("channel", -1); // Balance (Stereo)
    f.set("split", (c.pan + 100.0) / 200.0);
    clip.attach(f);
}

void decorate(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind)
{
    applyEffects(profile, cut, c);
    if (kind == TrackKind::Video) applyTransform(profile, cut, c);
    if (kind == TrackKind::Audio) {
        applyVolume(profile, cut, c);
        applyPan(profile, cut, c);
    }
}

// Ein-/Ausblenden über `len` Frames: Video über den Alphakanal (auf V1 = aus Schwarz, darüber = zur
// Spur darunter, wie DaVinci), Audio als Keyframe in dB pro Frame. equalPower: Kurve für den
// Audio-Crossfade (+3 dB wie DaVinci-Standard, Lautheit bleibt in der Mitte gleich).
// Keyframes per anim_set statt als Text: "0.5" würde sonst je nach LC_NUMERIC als 0 gelesen.
void applyFade(Mlt::Profile& profile, Mlt::Producer& cut, TrackKind kind, int len, bool fadeIn,
               bool equalPower = false)
{
    auto value = [&](int i) {
        const double t = fadeIn ? double(i) / len : double(len - 1 - i) / len;
        return equalPower ? std::sin(t * M_PI / 2) : t;
    };
    const bool video = kind == TrackKind::Video;
    Mlt::Filter f(profile, video ? "brightness" : "volume");
    if (!f.is_valid()) return;
    if (video) {
        f.set("level", 1.0);
        f.anim_set("alpha", value(0), 0, len);
        f.anim_set("alpha", value(len - 1), len - 1, len);
    } else {
        for (int i = 0; i < len; ++i) {
            const double v = value(i);
            f.anim_set("level", v > 0.001 ? 20.0 * std::log10(v) : -200.0, i, len);
        }
    }
    f.set_in_and_out(cut.get_in(), cut.get_out()); // Keyframes zählen ab Filter-In
    cut.attach(f);
}

} // namespace

TimelineBuilder::TimelineBuilder(Mlt::Profile& profile) : m_profile(profile) {}
TimelineBuilder::~TimelineBuilder() = default;

Mlt::Producer* TimelineBuilder::producerFor(const QString& path, TrackKind kind, int trackIndex, bool second)
{
    const QString key = QString("%1%2|%3|%4").arg(kind == TrackKind::Video ? "v" : "a").arg(second ? "x" : "")
                            .arg(trackIndex).arg(path);
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

std::unique_ptr<Mlt::Tractor> TimelineBuilder::build(const Timeline& tl)
{
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
            Mlt::Producer* p = producerFor(path, kind, trackIndex);
            return p && !isStill(p) ? p->get_length() : 0;
        };
        const QVector<TimelineOps::TransitionSpan> spans = TimelineOps::transitions(track, srcLen);
        QHash<int, const Clip*> byId;
        for (const Clip& c : track.clips) byId.insert(c.id, &c);

        // Ausschnitt [from, to) der Timeline aus Clip c (darf über In/Out hinaus in die Handles reichen)
        auto cutOf = [&](const Clip& c, int from, int to, bool second) -> Mlt::Producer* {
            Mlt::Producer* src = c.enabled ? producerFor(c.mediaPath, kind, trackIndex, second) : nullptr;
            if (!src) return nullptr;
            int in = c.in + (from - c.start);
            if (in < 0 && isStill(src)) in = 0; // Standbild: jedes Frame gleich
            if (in < 0) return nullptr;
            Mlt::Producer* cut = src->cut(in, in + (to - from) - 1);
            decorate(m_profile, *cut, c, kind);
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
            if (ca && cb) {
                // Cross Dissolve: beide Seiten in einem kleinen Tractor, Überblendung von a nach b
                Mlt::Tractor mix(m_profile);
                mix.set_track(*ca, 0);
                mix.set_track(*cb, 1);
                Mlt::Transition t(m_profile, kind == TrackKind::Video ? "luma" : "mix");
                if (kind == TrackKind::Audio) {
                    // Crossfade +3 dB: beide Seiten mit eigener Kurve, dann einfach addieren
                    applyFade(m_profile, *ca, kind, len, false, true);
                    applyFade(m_profile, *cb, kind, len, true, true);
                    t.set("start", 1.0);
                    t.set("sum", 1);
                }
                t.set_in_and_out(0, len - 1);
                mix.plant_transition(t, 0, 1);
                pl.append(mix);
            } else if (ca || cb) {
                // Nur eine Seite (Schnitt zum Leeren oder anderer Clip deaktiviert/offline): Aus-/Einblenden
                Mlt::Producer& one = ca ? *ca : *cb;
                applyFade(m_profile, one, kind, len, !ca);
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
        if (kind == TrackKind::Audio && track.muted) hide |= 2;
        pl.set("hide", hide);
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
    return tractor;
}
