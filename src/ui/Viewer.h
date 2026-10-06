#pragma once
#include "core/TrimFrames.h"

#include <QImage>
#include <QWidget>

class Engine;
class QLabel;
class QMimeData;
class QStackedWidget;
class TrimView;
class QToolButton;

// Vorschaufenster mit Transportleiste (Mitte oben wie in DaVinci).
// Zeigt Timeline oder Quelle (Media-Pool-Clip); Scrubber unter dem Bild mit In/Out-Bereich.
// Quelle: Bild in die Timeline ziehen = Quell-In/Out dort überschreiben (wie DaVinci).
class Viewer : public QWidget {
    Q_OBJECT
public:
    // Drag aus dem Viewer: Pfad wie beim Media Pool (MediaPool::MimeType), dazu der Quellbereich "in out"
    static constexpr const char* RangeMimeType = "application/x-schneidi-source-range";

    explicit Viewer(Engine* engine, QWidget* parent = nullptr);

    // Scrubber: Länge der gezeigten Quelle/Timeline und In/Out (-1 = nicht gesetzt)
    void setRange(int length, int markIn, int markOut);
    // Gezeigte Quelle (Titel über dem Bild, Drag in die Timeline); leer = Timeline
    void setSource(const QString& path, const QString& name);

    // Trim view like DaVinci: while an edit is dragged, show the frames on both sides of it (two-up/four-up,
    // fetched in the background via Engine::stills()); endTrim() goes back to the normal picture
    void showTrim(const TrimFrames::View& view);
    void endTrim();
    bool trimShown() const;
    TrimView* trimView() const { return m_trim; }

private:
    void updateTimecode(int frame);
    void updateModeText(); // "Timeline"/"Quelle – …", Hinweis bei umgangener Farbkorrektur
    QMimeData* dragData() const;

    Engine* m_engine;
    class Screen* m_screen;
    TrimView* m_trim;
    QStackedWidget* m_stack;
    class Scrubber* m_scrubber;
    QLabel* m_mode;
    QLabel* m_timecode;
    QToolButton* m_playBtn;
    QString m_sourcePath, m_sourceName;
    int m_length = 0, m_in = -1, m_out = -1;
};
