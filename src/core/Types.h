#pragma once
// Reines Datenmodell der Timeline – ohne Qt-Widgets und ohne MLT,
// damit Engine und UI unabhängig davon austauschbar bleiben.

#include <QString>
#include <QVariantMap>
#include <QVector>

enum class TrackKind { Video, Audio };

struct EffectInstance {
    QString effectId;   // Schlüssel in der EffectRegistry, z. B. "chromakey"
    QVariantMap params; // Parameter-Werte (fehlende = Default aus der Registry)
    bool enabled = true;
};

constexpr double kMinVolumeDb = -60.0; // ganz unten = -∞ (stumm)
constexpr double kMaxVolumeDb = 12.0;

// Anzeige wie in DaVinci: "+1.5 dB", "0.0 dB", "-∞ dB"
inline QString formatVolumeDb(double db)
{
    if (db <= kMinVolumeDb) return QStringLiteral("-∞ dB");
    return QString("%1%2 dB").arg(db > 0.05 ? "+" : "").arg(db, 0, 'f', 1);
}

// Transform/Crop/Composite wie im DaVinci-Inspector (Pixelwerte beziehen sich aufs Projektformat)
struct ClipTransform {
    double zoomX = 1.0, zoomY = 1.0;
    double posX = 0, posY = 0;  // Pixel, Y nach oben positiv (wie DaVinci)
    double rotation = 0;        // Grad
    double cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0; // Pixel
    double opacity = 100;       // Prozent
    // Bereiche einzeln abschaltbar (roter Punkt im Inspector); Werte bleiben erhalten
    bool transformOn = true, cropOn = true, compositeOn = true;

    bool hasTransform() const
    {
        return transformOn && (zoomX != 1.0 || zoomY != 1.0 || posX != 0 || posY != 0 || rotation != 0);
    }
    bool hasCrop() const
    {
        return cropOn && (cropLeft != 0 || cropRight != 0 || cropTop != 0 || cropBottom != 0);
    }
    bool hasOpacity() const { return compositeOn && opacity != 100; }
    bool isIdentity() const { return !hasTransform() && !hasCrop() && !hasOpacity(); }
};

struct Clip {
    int id = 0;
    QString mediaPath;
    int start = 0;  // Position in der Timeline (Frames)
    int in = 0;     // erstes Frame im Quellmaterial
    int out = 0;    // letztes Frame im Quellmaterial (inklusive, wie bei MLT)
    int linkId = 0; // 0 = frei; gleiche linkId = verknüpftes Video+Audio
    double volumeDb = 0.0; // Clip-Lautstärke (nur Audio); <= kMinVolumeDb = stumm
    double pan = 0.0;      // Nur Audio: -100 = links, 0 = Mitte, +100 = rechts
    bool enabled = true;   // deaktiviert (Taste D) = unsichtbar/stumm, bleibt aber liegen
    ClipTransform transform; // nur Video
    QVector<EffectInstance> effects;

    int length() const { return out - in + 1; }
    int end() const { return start + length(); } // exklusiv
};

struct Track {
    TrackKind kind = TrackKind::Video;
    QString name;
    QVector<Clip> clips; // immer nach start sortiert, ohne Überlappung
    bool muted = false;
    bool hidden = false;
};

struct TrackRef {
    TrackKind kind = TrackKind::Video;
    int index = 0; // V1 = Video/0, A1 = Audio/0
    bool operator==(const TrackRef& o) const { return kind == o.kind && index == o.index; }
};

struct Timeline {
    QVector<Track> video; // [0] = V1 (unterste Spur)
    QVector<Track> audio; // [0] = A1
    QVector<int> markers; // Timeline-Marker (Frames, sortiert)

    QVector<Track>& tracks(TrackKind k) { return k == TrackKind::Video ? video : audio; }
    const QVector<Track>& tracks(TrackKind k) const { return k == TrackKind::Video ? video : audio; }
    Track& track(TrackRef r) { return tracks(r.kind)[r.index]; }
    const Track& track(TrackRef r) const { return tracks(r.kind)[r.index]; }
};

struct MediaInfo {
    QString path;
    QString name;
    int length = 0; // Frames in Projekt-Framerate
    bool hasVideo = false;
    bool hasAudio = false;
    bool isImage = false;
};
