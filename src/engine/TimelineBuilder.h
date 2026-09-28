#pragma once
#include "core/Types.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace Mlt {
class Filter;
class Profile;
class Producer;
class Tractor;
} // namespace Mlt

// Übersetzt das Timeline-Modell in einen MLT-Tractor:
//   Spur 0 = schwarzer Hintergrund, dann V1..Vn (übereinander komponiert), dann A1..An (gemischt).
// Das Modell bleibt die Wahrheit; der Tractor wird bei Änderungen neu gebaut.

// Mixer-Anschlüsse für die Vorschau: Fader/Pan-Filter (live verstellbar, ohne Neuaufbau)
// und Pegelmesser (MLT "audiolevel", nach Fader/Pan). Der Export braucht sie nicht.
struct MixerHooks {
    struct Strip {
        std::shared_ptr<Mlt::Filter> volume, pan, meter;
        bool audible = true; // false = stumm/weggesoloed -> Pegel -∞
    };
    std::vector<Strip> tracks; // pro Audiospur
    Strip master;
};

class TimelineBuilder {
public:
    // Welche Datei für einen Clip gelesen wird (Vorschau: Proxy statt Original); ohne = immer das Original
    using MediaResolver = std::function<QString(const QString& path, TrackKind kind)>;

    explicit TimelineBuilder(Mlt::Profile& profile);
    ~TimelineBuilder();

    void setResolver(MediaResolver resolver) { m_resolver = std::move(resolver); }

    // hooks != nullptr: Mixer-Filter immer anhängen (auch bei 0 dB) und Pegelmesser einbauen
    std::unique_ptr<Mlt::Tractor> build(const Timeline& tl, MixerHooks* hooks = nullptr);
    // Fader/Pan live auf die Filter übertragen (Spuranzahl muss passen)
    static bool applyMixer(const Timeline& tl, const MixerHooks& hooks);

private:
    // second: eigener Producer für die einblendende Seite eines Übergangs (sonst spult ein Decoder
    // bei zwei Stellen derselben Datei hin und her)
    Mlt::Producer* producerFor(const QString& path, TrackKind kind, int trackIndex, bool second = false);

    Mlt::Profile& m_profile;
    MediaResolver m_resolver;
    // Pro Datei *und* Spur ein eigener Producer, damit sich Video- und Audiospur
    // nicht gegenseitig im Decoder hin- und herspulen.
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_cache;
    std::set<QString> m_used; // im aktuellen build() benutzte Schlüssel; der Rest fliegt danach raus
};
