#pragma once
// Vorschaubilder (Filmstreifen) und Wellenformen für die Timeline.
// Berechnet asynchron in zwei eigenen Threads, die UI fragt nur ab und
// bekommt über updated() Bescheid, wenn etwas Neues fertig ist.

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

    // Liefert das Bild zu (Datei, Quell-Frame), falls schon berechnet; sonst wird es
    // angefordert und ein leeres Bild zurückgegeben.
    QImage thumbnail(const QString& path, int frame);
    // Nur nachsehen, nichts anfordern (für Ersatzbilder aus gröberen Stufen)
    QImage cachedThumbnail(const QString& path, int frame);

    // Wellenform (evtl. noch unvollständig) oder nullptr; fordert sie ggf. an.
    std::shared_ptr<const Waveform> waveform(const QString& path);

signals:
    void updated(); // queued aus den Worker-Threads

private:
    struct ThumbJob {
        QString path;
        int frame;
    };
    static QString thumbKey(const QString& path, int frame);
    void thumbLoop();
    void waveLoop();

    QMutex m_mutex;
    QWaitCondition m_thumbCond;
    QWaitCondition m_waveCond;
    bool m_quit = false;

    QCache<QString, QImage> m_thumbs;
    std::deque<ThumbJob> m_thumbJobs; // vorne = neueste Anfrage (wird zuerst bearbeitet)
    QSet<QString> m_thumbPending;

    QHash<QString, std::shared_ptr<const Waveform>> m_waves;
    std::deque<QString> m_waveJobs;
    QSet<QString> m_waveRequested;

    QThread* m_thumbThread = nullptr;
    QThread* m_waveThread = nullptr;
};
