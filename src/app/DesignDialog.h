#pragma once
#include "app/Theme.h"

#include <QDialog>
#include <QPair>

class QComboBox;
class QLabel;
class QToolButton;
class DesignPreview;

// Arbeitsbereich → Design…: Design wählen, Primär-/Sekundärfarbe je Design anpassen, Signalfarben (für alle Designs
// gleich) einzeln einstellen. Vorschau im Dialog; gespeichert wird bei OK, wirksam nach dem Neustart (wie die Sprache).
class DesignDialog : public QDialog {
    Q_OBJECT
public:
    explicit DesignDialog(QWidget* parent = nullptr);

    QString design() const { return m_design; }
    Theme::Colors colors() const; // Farben so, wie sie nach dem Neustart gelten

    // Testhilfen (ohne Farbwahl-Dialog)
    void selectDesign(const QString& id);
    void setColor(const QString& key, const QColor& c);
    void resetDesignColor(const QString& key); // "primary" oder "secondary"
    void resetSignals();

    void accept() override;

private:
    void pick(const QString& key);
    void refresh();

    QString m_design;
    QHash<QString, QPair<QColor, QColor>> m_custom; // je Design: Primär-, Sekundärfarbe
    Theme::Colors m_signals;
    QComboBox* m_designs;
    QLabel* m_description;
    DesignPreview* m_preview;
    QHash<QString, QToolButton*> m_swatches;
    QHash<QString, QToolButton*> m_resets; // "primary", "secondary", "signals"
};
