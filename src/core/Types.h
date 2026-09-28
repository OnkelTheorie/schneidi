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

struct Clip {
    int id = 0;
    QString mediaPath;
    int start = 0;  // Position in der Timeline (Frames)
    int in = 0;     // erstes Frame im Quellmaterial
    int out = 0;    // letztes Frame im Quellmaterial (inklusive, wie bei MLT)
    int linkId = 0; // 0 = frei; gleiche linkId = verknüpftes Video+Audio
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
