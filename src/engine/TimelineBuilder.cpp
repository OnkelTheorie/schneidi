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
            crop.set("rect", QString("%1 %2 %3 %4").arg(t.cropLeft).arg(t.cropTop).arg(w).arg(h).toUtf8().constData());
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
    // 5. Wert (Deckkraft) muss dabei sein, sonst wird das Bild unsichtbar
    f.set("rect", QString("%1 %2 %3 %4 1").arg(x).arg(y).arg(w).arg(h).toUtf8().constData());
    f.set("rotation", -t.rotation); // DaVinci: positiv = gegen den Uhrzeigersinn
    f.set("rotate_center", 1);
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
