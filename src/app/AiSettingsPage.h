#pragma once
#include "app/AiSettings.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QToolButton;
class QVBoxLayout;

// Settings › AI: one section (group box with a form) per topic. The AI chat (plan 0.12, point 4) adds its own
// section "AI chat" (provider, address, model, key, approval mode, test button) with addSection(…, 0), so it comes
// first; the sections below are about every AI connected to the app (schneidi-cli, MCP, the chat itself).
class AiSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit AiSettingsPage(const AiSettings& settings, QWidget* parent = nullptr);
    AiSettings settings() const; // as chosen on the page

    // New section at `index` (-1 = at the end); fill its form layout (QGroupBox::layout() is a QFormLayout)
    QGroupBox* addSection(const QString& title, int index = -1);
    static QFormLayout* formOf(QGroupBox* section);

    void setColor(const QColor& c); // also the test helper (without the color dialog)

private:
    void updateEnabled();

    AiSettings m_initial; // what the page does not show stays as it was
    QVBoxLayout* m_sections = nullptr;
    QCheckBox* m_liveAllowed = nullptr;
    QCheckBox* m_highlight = nullptr;
    QToolButton* m_colorButton = nullptr;
    QToolButton* m_colorReset = nullptr;
    QColor m_color;
    QDoubleSpinBox* m_duration = nullptr;
    QComboBox* m_indicator = nullptr;
};
