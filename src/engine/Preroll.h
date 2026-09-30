#pragma once
#include "engine/TimelineBuilder.h"

#include <QSize>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Vorschau: Decoder (Bild und Ton) vor einer Sprungstelle (Clip-Anfang, Übergang) im Hintergrund an die Stelle bringen.
// Bei Material mit langen GOPs (OBS, Handy) muss der Decoder beim Springen bis zu mehrere Sekunden ab dem letzten
// Keyframe dekodieren – mitten in der Wiedergabe stand das Bild dann bis zu ~1 s. Hier wird das Frame vor der Stelle
// rechtzeitig dekodiert, danach liest die Wiedergabe am Stück weiter. Nur bei Vorwärts-Wiedergabe.
class Preroll {
public:
    Preroll();
    ~Preroll();

    // Nach jedem Neuaufbau der Vorschau (Stellen aus TimelineBuilder::prerollPoints)
    void setPoints(std::vector<PrerollPoint> points, int lookahead, QSize size);
    // Gezeigtes Frame (Consumer-Thread) und Tempo; speed <= 0 = nichts vorbereiten
    void update(int position, double speed);
    void clear();

private:
    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<PrerollPoint> m_points;
    std::vector<bool> m_done;
    int m_lookahead = 0; // Frames
    QSize m_size;
    int m_position = 0;
    double m_speed = 0.0;
    bool m_quit = false;
    std::thread m_thread;
};
