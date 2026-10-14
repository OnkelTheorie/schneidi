#pragma once
#include "app/Theme.h"

#include <QDialog>
#include <QPair>

class QComboBox;
class QLabel;
class QToolButton;
class DesignPreview;

// Workspace → Design…: choose a design, adjust primary/secondary color per design, set signal colors (same for all designs)
// individually. Preview in the dialog; saved on OK, effective after restart (like the language).
class DesignDialog : public QDialog {
    Q_OBJECT
public:
    explicit DesignDialog(QWidget* parent = nullptr);

    QString design() const { return m_design; }
    Theme::Colors colors() const; // Colors as they apply after the restart

    // Test helpers (without color picker dialog)
    void selectDesign(const QString& id);
    void setColor(const QString& key, const QColor& c);
    void resetDesignColor(const QString& key); // "primary" or "secondary"
    void resetSignals();

    void accept() override;

private:
    void pick(const QString& key);
    void refresh();

    QString m_design;
    QHash<QString, QPair<QColor, QColor>> m_custom; // per design: primary, secondary color
    Theme::Colors m_signals;
    QComboBox* m_designs;
    QLabel* m_description;
    DesignPreview* m_preview;
    QHash<QString, QToolButton*> m_swatches;
    QHash<QString, QToolButton*> m_resets; // "primary", "secondary", "signals"
};
