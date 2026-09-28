#pragma once
#include <QWidget>

class Editor;
class QLabel;

// Inspector (rechts oben wie in DaVinci). Zeigt vorerst nur Infos zum ausgewählten Clip;
// Lautstärke, Fades, Green Screen folgen hier (Parameter aus der EffectRegistry).
class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(Editor* editor, QWidget* parent = nullptr);

private:
    void refresh();

    Editor* m_editor;
    QLabel* m_body;
};
