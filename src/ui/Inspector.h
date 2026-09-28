#pragma once
#include "core/Types.h"

#include <QSize>
#include <QWidget>
#include <functional>

class Editor;
class QButtonGroup;
class QGridLayout;
class QLabel;
class QSlider;
class QStackedWidget;
class QToolButton;
class QVBoxLayout;
class ScrubField;

// Inspector (rechts oben) im Stil von DaVinci: Tabs Video/Audio, aufklappbare Bereiche mit
// rotem An/Aus-Punkt, Zahlenfelder zum Eintippen oder Links/Rechts-Ziehen.
// Änderungen gelten für alle ausgewählten Clips der Art; Ziehen = ein Undo-Schritt.
class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(Editor* editor, QWidget* parent = nullptr);
    ~Inspector() override;
    void setFrameSize(const QSize& size);

private:
    using Field = std::function<double&(Clip&)>;
    using Flag = std::function<bool&(Clip&)>;

    struct Param {
        TrackKind kind;
        Field field;
        ScrubField* edit;
        QSlider* slider = nullptr;
        double min, max;
    };
    struct Section {
        QGridLayout* grid;
        TrackKind kind;
        int rows = 0;
    };

    // Bereich mit Kopfzeile; `enabled` = roter Punkt (optional)
    Section addSection(QVBoxLayout* page, TrackKind kind, const QString& title, const std::function<void(Clip&)>& reset,
                       const Flag& enabled = {});
    // Zeile mit Schieberegler + Zahlenfeld
    Param* addSlider(Section& s, const QString& key, const QString& label, double min, double max, double def,
                     double step, int decimals, const Field& field);
    // Zeile mit X/Y-Feldern (wie Zoom/Position in DaVinci); link = Kettensymbol dazwischen
    QPair<Param*, Param*> addXY(Section& s, const QString& key, const QString& label, double min, double max,
                                double def, double step, int decimals, const Field& x, const Field& y,
                                QToolButton* link = nullptr);
    Param* makeParam(TrackKind kind, const QString& key, const QString& text, double min, double max, double def,
                     double step, int decimals, const Field& field);
    QToolButton* resetButton(const std::function<void()>& fn);
    QLabel* rowLabel(const QString& text);

    QVector<int> selectedIds(TrackKind kind) const;
    const Clip* primary(TrackKind kind) const;
    void apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn);
    void refresh();

    Editor* m_editor;
    QSize m_frameSize{1920, 1080};
    QLabel* m_clipName;
    QLabel* m_empty;
    QWidget* m_content;
    QStackedWidget* m_pages;
    QButtonGroup* m_tabs;
    QVector<Param*> m_params;
    QVector<std::function<void()>> m_refreshers; // weitere Anzeigen (Punkte, Farbe)
    bool m_zoomLinked = true;
    Param* m_crop[4] = {};
};
