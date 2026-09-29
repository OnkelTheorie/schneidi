#pragma once
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class Engine;
class Project;
class QHBoxLayout;
class QTimer;
class ChannelStrip;
class LoudnessStrip;

// Mixer wie in DaVincis Edit-Page (abgespeckt): pro Audiospur ein Kanalzug mit Pegel,
// Fader, Pan, Mute/Solo, rechts der Master und ein Loudness-Meter (LUFS). Fader ziehen = ein Undo-Schritt.
class Mixer : public QWidget {
    Q_OBJECT
public:
    Mixer(Project* project, Engine* engine, QWidget* parent = nullptr);

private:
    void sync();         // Modell -> Kanalzüge (legt sie bei geänderter Spuranzahl neu an)
    void rebuildStrips(int count);
    void onLevels(const QVector<float>& db);
    void tick();         // Pegel abfallen lassen, Peak-Hold

    Project* m_project;
    Engine* m_engine;
    QHBoxLayout* m_stripLayout = nullptr;
    QVector<ChannelStrip*> m_strips;
    ChannelStrip* m_master = nullptr;
    LoudnessStrip* m_loudness = nullptr; // Loudness-Meter rechts neben dem Master
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock, m_lastLevels;
};
