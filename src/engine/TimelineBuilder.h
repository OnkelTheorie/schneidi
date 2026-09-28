#pragma once
#include "core/Types.h"

#include <map>
#include <memory>

namespace Mlt {
class Profile;
class Producer;
class Tractor;
} // namespace Mlt

// Übersetzt das Timeline-Modell in einen MLT-Tractor:
//   Spur 0 = schwarzer Hintergrund, dann V1..Vn (übereinander komponiert), dann A1..An (gemischt).
// Das Modell bleibt die Wahrheit; der Tractor wird bei Änderungen neu gebaut.
class TimelineBuilder {
public:
    explicit TimelineBuilder(Mlt::Profile& profile);
    ~TimelineBuilder();

    std::unique_ptr<Mlt::Tractor> build(const Timeline& tl);

private:
    // second: eigener Producer für die einblendende Seite eines Übergangs (sonst spult ein Decoder
    // bei zwei Stellen derselben Datei hin und her)
    Mlt::Producer* producerFor(const QString& path, TrackKind kind, int trackIndex, bool second = false);

    Mlt::Profile& m_profile;
    // Pro Datei *und* Spur ein eigener Producer, damit sich Video- und Audiospur
    // nicht gegenseitig im Decoder hin- und herspulen.
    std::map<QString, std::unique_ptr<Mlt::Producer>> m_cache;
};
