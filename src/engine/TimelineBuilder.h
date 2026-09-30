#pragma once
#include "core/Types.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

class ProducerFactory;

namespace Mlt {
class Filter;
class Profile;
class Producer;
class Tractor;
class Playlist;
} // namespace Mlt

// Übersetzt das Timeline-Modell in einen MLT-Tractor:
//   Spur 0 = schwarzer Hintergrund, dann V1..Vn (übereinander komponiert), dann A1..An (gemischt).
// Das Modell bleibt die Wahrheit; der Tractor wird bei Änderungen neu gebaut.

// Mixer-Anschlüsse für die Vorschau: Fader/Pan-Filter (live verstellbar, ohne Neuaufbau)
// und Pegelmesser (eigener Filter, nach Fader/Pan/Limiter). Der Export braucht sie nicht.
struct MixerHooks {
    struct Strip {
        std::shared_ptr<Mlt::Filter> volume, pan, meter;
        std::shared_ptr<Mlt::Filter> limiter; // nur Master: immer angehängt, per "disable" an/aus
        bool audible = true; // false = stumm/weggesoloed -> Pegel -∞
        std::shared_ptr<Mlt::Playlist> playlist; // nur Spuren: Mute/Solo live über "hide" (ohne Neuaufbau)
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
    // Vorschau „Vorher/Nachher“ (Color-Seite): true = Farbkorrektur umgehen; wirkt live ohne Neuaufbau.
    // Ohne (Export, Standbild) wird immer korrigiert.
    void setGradeBypass(std::shared_ptr<std::atomic<bool>> flag) { m_gradeBypass = std::move(flag); }
    // Render-Cache (nur Vorschau, engine/RenderCache.h): fertig gerenderte Ausgabe eines Videoclips (Datei) oder
    // leer = normal aus dem Original mit Effekten. Ohne = nie (Export, Standbild).
    using ClipCache = std::function<QString(const Clip& clip)>;
    void setClipCache(ClipCache cache) { m_clipCache = std::move(cache); }

    // Untertitel (sichtbare Untertitelspur) mit ins Bild; Standard an (Vorschau), Export nur beim Einbrennen
    void setSubtitles(bool on) { m_subtitles = on; }

    // hooks != nullptr: Mixer-Filter immer anhängen (auch bei 0 dB) und Pegelmesser einbauen
    std::unique_ptr<Mlt::Tractor> build(const Timeline& tl, MixerHooks* hooks = nullptr);
    // Fader/Pan/Limiter live auf die Filter übertragen (Spuranzahl muss passen)
    static bool applyMixer(const Timeline& tl, MixerHooks& hooks);
    // Ton eines Clips so, wie ihn die Timeline liest (Geschwindigkeit/Rückwärts eingerechnet), ohne
    // Clip-Lautstärke/Fades; Frames in..out des Clips. nullptr = Titel/Standbild/Datei fehlt.
    // Gehört dem Builder (Cache), gilt bis zum nächsten Aufruf.
    Mlt::Producer* clipAudioSource(const Clip& c);
    // Bild eines einzelnen Videoclips mit Effekten, Transform, Geschwindigkeit und Keyframes (ohne Fades und
    // Übergänge) auf transparentem Grund; Frame 0 = erstes Clip-Frame. Für den Render-Cache.
    std::unique_ptr<Mlt::Tractor> buildClipOutput(const Clip& c);

private:
    // second: eigener Producer für die einblendende Seite eines Übergangs (sonst spult ein Decoder
    // bei zwei Stellen derselben Datei hin und her)
    // retime: Clip, dessen Geschwindigkeit/Rückwärts/Standbild und Ton-Stream gilt (nullptr = Originaltempo, Stream 0)
    Mlt::Producer* producerFor(const QString& path, TrackKind kind, int trackIndex, bool second = false,
                               const Clip* retime = nullptr);

    // Eine Timeline ohne Aufräumen des Producer-Caches (auch für verschachtelte Sequenzen)
    std::unique_ptr<Mlt::Tractor> buildTimeline(const Timeline& tl, MixerHooks* hooks);
    // Compound Clip: Inhalt der Sequenz (aus Timeline::nested) als Tractor; nullptr = fehlt/Schleife
    Mlt::Producer* nestedProducer(int sequenceId, TrackKind kind, int trackIndex, bool second);

    Mlt::Profile& m_profile;
    std::unique_ptr<ProducerFactory> m_factory; // vor m_cache: muss die Producer überleben
    MediaResolver m_resolver;
    std::shared_ptr<std::atomic<bool>> m_gradeBypass;
    ClipCache m_clipCache;
    bool m_subtitles = true;
    bool m_transparent = false; // Hintergrund durchsichtig statt schwarz (buildClipOutput)
    // Pro Datei *und* Spur ein eigener Producer, damit sich Video- und Audiospur
    // nicht gegenseitig im Decoder hin- und herspulen.
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_cache;
    std::set<QString> m_used; // im aktuellen build() benutzte Schlüssel; der Rest fliegt danach raus
    // Verschachtelte Timelines (Compound Clips) des laufenden build()
    std::shared_ptr<const NestedTimelines> m_nested;
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_nestedCache;
    std::set<int> m_nestStack; // gerade gebaute Sequenzen (Schleifenschutz)
    bool m_inNested = false;
    QString m_keyPrefix; // Cache-Schlüssel-Präfix innerhalb verschachtelter Sequenzen
};
