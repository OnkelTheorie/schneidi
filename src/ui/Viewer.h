#pragma once
#include <QImage>
#include <QWidget>

class Engine;
class QLabel;
class QToolButton;

// Vorschaufenster mit Transportleiste (Mitte oben wie in DaVinci).
class Viewer : public QWidget {
    Q_OBJECT
public:
    explicit Viewer(Engine* engine, QWidget* parent = nullptr);

private:
    void updateTimecode(int frame);

    Engine* m_engine;
    class Screen* m_screen;
    QLabel* m_mode;
    QLabel* m_timecode;
    QToolButton* m_playBtn;
};
