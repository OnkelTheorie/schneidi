#pragma once
#include "app/AiKeys.h"
#include "app/AiProvider.h"
#include "app/AiSettings.h"

#include <QJsonObject>
#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QToolButton;
class QVBoxLayout;
class QLineEdit;
class AiClient;

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

    // "Connect with Claude" (Claude Code + Claude Desktop, cli/Setup.h): the app hands in the setup (the page does not
    // depend on the command layer); without it there is no such section. Result per target as Cli::Setup::claude
    using ClaudeSetup = std::function<QJsonObject(bool remove)>;
    void setClaudeSetup(const ClaudeSetup& setup);
    // Runs it in a thread and shows the answer below the buttons (the buttons ask first; tests call this directly)
    void runClaudeSetup(bool remove);
    static QString claudeResultText(const QJsonObject& result, bool remove); // one line per target, translated

    // The AI chat's section, first on the page: provider, address, model (list from the provider), key (where it
    // comes from, stored in the keychain), approval mode, connection test, what a company file prescribes. `keys`
    // stays the main window's; a typed key is stored only by applyChatKey() (after OK)
    void addChatSection(Ai::Keys* keys);
    bool applyChatKey(QString* error);
    QString chatProvider() const; // chosen on the page

private:
    void updateEnabled();
    void chatProviderChanged();
    void updateKeyState();
    Ai::Endpoint chatEndpoint(); // from the page's fields (a typed key or the stored one)

    AiSettings m_initial; // what the page does not show stays as it was
    QVBoxLayout* m_sections = nullptr;
    QCheckBox* m_liveAllowed = nullptr;
    QSpinBox* m_release = nullptr;
    QCheckBox* m_highlight = nullptr;
    QToolButton* m_colorButton = nullptr;
    QToolButton* m_colorReset = nullptr;
    QColor m_color;
    QDoubleSpinBox* m_duration = nullptr;
    QCheckBox* m_distinguish = nullptr;
    QComboBox* m_indicator = nullptr;
    QSpinBox* m_logLength = nullptr;
    ClaudeSetup m_claudeSetup;
    QPushButton* m_claudeConnect = nullptr;
    QPushButton* m_claudeRemove = nullptr;
    QLabel* m_claudeResult = nullptr;
    // AI chat
    Ai::Keys* m_keys = nullptr;
    Ai::Company m_company;
    QComboBox* m_chatProvider = nullptr;
    QLineEdit* m_chatAddress = nullptr;
    QComboBox* m_chatModel = nullptr;
    QLineEdit* m_chatKey = nullptr;
    QLabel* m_keyState = nullptr;
    QPushButton* m_keyRemove = nullptr;
    QComboBox* m_approval = nullptr;
    QComboBox* m_compact = nullptr;
    QLabel* m_chatResult = nullptr;
    AiClient* m_client = nullptr;
    QHash<QString, QString> m_models; // per provider, as edited on the page
    QString m_shownProvider;
    QString m_startProvider, m_startModel; // shown when the page opened
};
