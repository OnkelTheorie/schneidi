#pragma once
#include "core/Types.h"

#include <QSize>
#include <QWidget>
#include <functional>

class Editor;
class QLabel;
class QTabWidget;
class QVBoxLayout;
class QCheckBox;
class QToolButton;
class ParamRow;

// Inspector (rechts oben wie in DaVinci): Eigenschaften des ausgewählten Clips bearbeiten.
// Tabs Video/Audio; Änderungen gelten für alle ausgewählten Clips der Art.
// Regler ziehen = ein Undo-Schritt (Project::edit mit mergeKey).
class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(Editor* editor, QWidget* parent = nullptr);
    void setFrameSize(const QSize& size);

private:
    // Regler für ein double-Feld des Clips
    ParamRow* addParam(QVBoxLayout* section, TrackKind kind, const QString& key, const QString& label, double min,
                       double max, double def, int decimals, const std::function<double&(Clip&)>& field);
    QVBoxLayout* addSection(QVBoxLayout* page, const QString& title, const std::function<void()>& reset,
                            QCheckBox* enable = nullptr);
    QVector<int> selectedIds(TrackKind kind) const;
    const Clip* primary(TrackKind kind) const;
    void apply(TrackKind kind, const QString& key, const QString& text, const std::function<void(Clip&)>& fn);
    void refresh();

    struct Binding {
        TrackKind kind;
        ParamRow* row;
        std::function<double&(Clip&)> field;
    };

    Editor* m_editor;
    QSize m_frameSize{1920, 1080};
    QLabel* m_clipName;
    QLabel* m_empty;
    QTabWidget* m_tabs;
    QWidget* m_videoPage;
    QWidget* m_audioPage;
    QVector<Binding> m_bindings;
    bool m_zoomLinked = true;
    QToolButton* m_zoomLink = nullptr;
    QCheckBox* m_keyEnabled = nullptr;
    QToolButton* m_keyColor = nullptr;
    ParamRow* m_keyTolerance = nullptr;
    ParamRow* m_cropRows[4] = {};
};
