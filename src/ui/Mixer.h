#pragma once
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class Editor;
class Engine;
class Project;
class QHBoxLayout;
class QTimer;
class ChannelStrip;
class LoudnessStrip;

// Mixer like DaVinci's Edit page (slimmed down): per audio track a channel strip with meter, fader, pan,
// mute/solo, EQ/DYN; on the right the master and a loudness meter (LUFS). Dragging a fader = one undo step.
class Mixer : public QWidget {
    Q_OBJECT
public:
    Mixer(Project* project, Engine* engine, QWidget* parent = nullptr);
    // EQ/compressor changes go through this editor (the main window's); without one the mixer makes its own
    void setEditor(Editor* editor);

protected:
    bool eventFilter(QObject* watched, QEvent* e) override; // double click on EQ/DYN opens the editor

private:
    Editor* editor();
    void sync();         // Modell -> Kanalzüge (legt sie bei geänderter Spuranzahl neu an)
    void rebuildStrips(int count);
    void fitStrips();    // strip area width: whole channel strips only
    int fixedWidth() const; // master + loudness
    void growToFit();    // widen the mixer in its splitter so all strips fit
    void resizeEvent(class QResizeEvent* e) override;
    void showEvent(class QShowEvent* e) override;
    void onLevels(const QVector<float>& db);
    void tick();         // Pegel abfallen lassen, Peak-Hold

    Project* m_project;
    Engine* m_engine;
    Editor* m_editor = nullptr;
    QHBoxLayout* m_stripLayout = nullptr;
    class QScrollArea* m_stripScroll = nullptr;
    QVector<ChannelStrip*> m_strips;
    ChannelStrip* m_master = nullptr;
    LoudnessStrip* m_loudness = nullptr; // Loudness-Meter rechts neben dem Master
    QTimer* m_timer = nullptr;
    QElapsedTimer m_clock, m_lastLevels;
};
