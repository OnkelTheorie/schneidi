#pragma once
#include "app/AiSettings.h"

#include <QDialog>

class AiSettingsPage;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTabWidget;
class UpdateCheck;

// Settings of the app (File › Settings…, Ctrl+,), the same for all projects; project settings stay in their own
// dialog. Tabs: General (language, design, keys, update hint, threads for background work) and AI (AiSettingsPage). Nothing is applied before OK
// ("Check now" for updates asks right away); the caller reads the choices and applies/saves them
// (MainWindow::settingsDialog). Options that already have a menu entry keep it (the menus stay where users of other
// editors look for them).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum class Tab { General, Ai };

    SettingsDialog(const AiSettings& ai, QWidget* parent = nullptr);
    void showTab(Tab tab);

    AiSettings aiSettings() const;
    QString language() const; // code, as I18n::language()
    bool updateHint() const;  // look for new versions at the start (UpdateCheck::enabled)
    int backgroundThreads() const; // Background::chosenThreads (0 = automatic)
    AiSettingsPage* aiPage() const { return m_aiPage; }

signals:
    // Buttons for the dialogs that already exist (the caller opens them, they take effect like from the menu)
    void designRequested();
    void keyBindingsRequested();

private:
    QWidget* buildGeneral();

    QTabWidget* m_tabs = nullptr;
    AiSettingsPage* m_aiPage = nullptr;
    QComboBox* m_language = nullptr;
    QComboBox* m_threads = nullptr;
    QCheckBox* m_updateHint = nullptr;
    QPushButton* m_checkNow = nullptr;
    QLabel* m_updateResult = nullptr;
    UpdateCheck* m_updates = nullptr;
};
