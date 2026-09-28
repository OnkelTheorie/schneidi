#include "app/Theme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace Theme {

void apply(QApplication& app)
{
    app.setStyle(QStyleFactory::create("Fusion")); // sieht unter Linux und Windows gleich aus

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, panel);
    p.setColor(QPalette::AlternateBase, panelHeader);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panelHeader);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::ToolTipBase, panelHeader);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, textDim);
    p.setColor(QPalette::Disabled, QPalette::Text, textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDim);
    app.setPalette(p);

    QFont f = app.font();
    f.setPointSizeF(9.0);
    app.setFont(f);

    app.setStyleSheet(QString(R"(
        QMainWindow, QWidget#Root { background: %1; }
        QSplitter::handle { background: %2; }
        QSplitter::handle:horizontal { width: 3px; }
        QSplitter::handle:vertical { height: 3px; }
        QWidget#Panel { background: %3; }
        QLabel#PanelTitle { color: %5; font-weight: 600; padding: 4px 8px; background: %4; }
        QToolButton { color: %5; background: transparent; border: none; padding: 3px 6px; border-radius: 3px; }
        QToolButton:hover { background: #3a3a42; }
        QToolButton:checked { color: %6; background: #3a3a42; }
        QListWidget { background: %3; border: none; }
        QListWidget::item:selected { background: #3a3a42; border: 1px solid %6; }
        QScrollBar { background: %1; border: none; }
        QScrollBar:horizontal { height: 10px; }
        QScrollBar:vertical { width: 10px; }
        QScrollBar::handle { background: #45454d; border-radius: 4px; min-width: 20px; min-height: 20px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QMenuBar { background: %1; color: %5; }
        QMenuBar::item:selected { background: #3a3a42; }
        QMenu { background: %4; color: %5; border: 1px solid %2; }
        QMenu::item:selected { background: #3a3a42; }
        QStatusBar { background: %1; color: %7; }
    )")
                          .arg(window.name(), border.name(), panel.name(), panelHeader.name(),
                               text.name(), accent.name(), textDim.name()));
}

} // namespace Theme
