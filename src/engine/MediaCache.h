#pragma once
// Vorschaubilder (Filmstreifen) und Wellenformen für die Timeline.
// Berechnet asynchron in eigenen Threads, die UI fragt nur ab und
// bekommt über updated() Bescheid, wenn etwas Neues fertig ist.
//
// Filmstreifen schnell: pro Datei ein ffmpeg-Durchlauf, der nur die Keyframes dekodiert (ein Bild je
// Keyframe, ~20 ms statt ~150–700 ms für ein genau gesuchtes 4K-Frame). Genaue Frames werden nur noch
// nachgeladen, wenn der Keyframe weiter weg liegt als der Kachelabstand (stark hineingezoomt).

#include "core/ProjectFormat.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWaitCondition>
#include <deque>
#include <memory>

class QThread;

struct Waveform {
    static constexpr int kBucketsPerFrame = 8;
    // levels[0]: ein Spitzenwert (0..255) pro Bucket, levels[k]: jeweils 2 Buckets zusammengefasst
    QVector<QVector<quint8>> levels;
    int frames = 0;     // so viele Frames sind schon berechnet
    bool complete = false;
};

class MediaCache : public QObject {
    Q_OBJECT
public:
    static constexpr int kThumbHeight = 90;

    explicit MediaCache(QObject* parent = nullptr);
    ~MediaCache() override;

    // Liefert das Bild zu (Datei, Quell-Frame), falls schon berechnet; sonst wird es angefordert.
    // Bis dahin kommt das Bild des vorigen Keyframes (oder ein leeres). tolerance: so viele Frames darf
    // der Keyframe vor dem gewünschten Frame liegen, ohne dass das genaue Frame berechnet wird.
    QImage thumbnail(const QString& path, int frame, int tolerance = 1);
    // Nur nachsehen, nichts anfordern (für Ersatzbilder aus gröberen Stufen)
    QImage cachedThumbnail(const QString& path, int frame);

    // Wellenform (evtl. noch unvollständig) oder nullptr; fordert sie ggf. an. stream: Ton-Stream der Datei (0 = erster)
    std::shared_ptr<const Waveform> waveform(const QString& path, int stream = 0);

    // Projekteinstellungen geändert: Frames zählen in der neuen Framerate, Bilder im neuen Seitenverhältnis
    // -> alles Berechnete verwerfen (Wellenformen auf der Platte sind nach Framerate getrennt)
    void setFormat(const ProjectFormat& format);

signals:
    void updated(); // queued aus den Worker-Threads

private:
    struct ThumbJob {
        QString path;
        int frame;
    };
    struct KeyIndex {
        QVector<int> frames; // Quell-Frames der schon dekodierten Keyframes (aufsteigend)
        bool done = false;
        bool failed = false; // ffmpeg ging nicht -> nur genaue Frames
    };
    static QString thumbKey(const QString& path, int frame);
    void queueThumb(const QString& path, int frame);
    void thumbLoop();
    void stripLoop();
    void waveLoop();

    QMutex m_mutex;
    QWaitCondition m_thumbCond;
    QWaitCondition m_waveCond;
    bool m_quit = false;
    ProjectFormat m_format;
    int m_generation = 0; // zählt Formatwechsel; ältere Ergebnisse der Threads werden verworfen

    QCache<QString, QImage> m_thumbs;
    std::deque<ThumbJob> m_thumbJobs; // vorne = neueste Anfrage (wird zuerst bearbeitet)
    QSet<QString> m_thumbPending;

    QHash<QString, KeyIndex> m_keys;
    std::deque<QString> m_stripJobs;
    QWaitCondition m_stripCond;

    QHash<QString, std::shared_ptr<const Waveform>> m_waves;
    std::deque<QString> m_waveJobs;
    QSet<QString> m_waveRequested;

    QThread* m_thumbThread = nullptr;
    QThread* m_stripThread = nullptr;
    QThread* m_waveThread = nullptr;
};
