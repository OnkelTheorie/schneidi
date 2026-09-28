#pragma once
#include "core/Types.h"

#include <QObject>
#include <QTimer>
#include <memory>

namespace Mlt {
class Profile;
class Consumer;
class Tractor;
} // namespace Mlt
class TimelineBuilder;

struct ExportSettings {
    QString path;
    QString profile = "atsc_1080p_25"; // MLT-Profil (Auflösung + fps)
    QString videoCodec = "libx264";
    QString audioCodec = "aac";
    int crf = 20;             // Qualität: kleiner = besser
    QString preset = "medium"; // Geschwindigkeit vs. Dateigröße
    int audioBitrateK = 192;
};

// Rendert die Timeline in eine Datei. Eigene Producer-Instanzen,
// damit Vorschau und Export sich nicht in die Quere kommen.
class Exporter : public QObject {
    Q_OBJECT
public:
    explicit Exporter(QObject* parent = nullptr);
    ~Exporter() override;

    bool isRunning() const { return m_consumer != nullptr; }
    bool start(const Timeline& tl, const ExportSettings& settings, QString* error);
    void cancel();

signals:
    void progress(int percent);
    void finished(bool ok, const QString& message);

private:
    void poll();
    void cleanup();

    std::unique_ptr<Mlt::Profile> m_profile;
    std::unique_ptr<TimelineBuilder> m_builder;
    std::unique_ptr<Mlt::Tractor> m_tractor;
    std::unique_ptr<Mlt::Consumer> m_consumer;
    QTimer m_timer;
    int m_length = 0;
    QString m_path;
};
