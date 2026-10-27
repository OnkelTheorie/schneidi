#pragma once
// The "AI" panel: chat with an AI that edits the open project (app/AiChat.h). Messages of the user and the AI, the
// AI's tool steps (one line each, opened with a click: arguments and answer; pictures as thumbnails), the input
// (Enter sends, Shift+Enter is a new line), Stop while it works, "New chat", "Copy" and the chat's tokens. Review
// mode: a bar "Accept / Discard" after a request that changed the project. Without a set-up provider: a short setup
// (provider, key, model) right here. MainWindow (MainWindowChat.cpp) decides what happens with a request (privacy
// notice, key, undo bookkeeping) and docks the panel left or right.

#include "app/AiChat.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

class AiChatPanel : public QWidget {
    Q_OBJECT
public:
    explicit AiChatPanel(AiChat* chat, QWidget* parent = nullptr);

    // A request typed by the user: true = it went out (the input empties), false = it stays in the input
    void setSender(const std::function<bool(const QString& text)>& send) { m_send = send; }
    void sendText(const QString& text); // like typing it and pressing Enter (tests)
    QPlainTextEdit* input() const { return m_input; }
    QString workingText() const; // the working line (tests)
    // A local model silent this long gets the line why (it reads the whole request first); tests set less
    void setSlowHintMs(int ms) { m_slowHintMs = ms; }

    // Setup page instead of the chat (no provider yet, or its key is missing). keyHint(provider): where the key
    // comes from when the user types none ("from ANTHROPIC_API_KEY"), empty = they have to give one
    struct SetupChoice {
        QString provider, key, model, address; // address: only for the own address ("custom")
    };
    void showSetup(bool on, const QString& provider = {}, const QString& message = {});
    bool setupShown() const;
    void setKeyHint(const std::function<QString(const QString& provider)>& hint) { m_keyHint = hint; }
    void setProviderText(const QString& text, const QString& tip); // the model; provider and address as tooltip
    void setDockedLeft(bool left);

    // Review mode: the request changed the project, the user decides (the input waits meanwhile)
    void showApproval(const QString& text);
    void hideApproval();
    bool approvalShown() const;

signals:
    void setupDone(const SetupChoice& choice);
    void settingsRequested();
    void sideRequested(bool left);
    void hideRequested();
    void approved();
    void discarded();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void submit();
    void elideProvider();
    void addEntry(int index);
    void updateEntry(int index);
    void rebuild();
    void flush(); // pending streamed text -> labels
    void updateBusy(bool busy);
    void updateUsage();
    void updateEmpty();
    void updateWorking(); // "the AI works …" while nothing else shows it
    void scrollDown(bool force);
    QWidget* buildSetup();
    void setupProviderChanged();

    AiChat* m_chat;
    std::function<bool(const QString&)> m_send;
    std::function<QString(const QString&)> m_keyHint;
    QStackedWidget* m_pages = nullptr;
    // Chat page
    QLabel* m_provider = nullptr;
    QString m_providerText;
    QToolButton* m_menuButton = nullptr;
    class QAction* m_left = nullptr;
    class QAction* m_right = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget* m_list = nullptr;
    QVBoxLayout* m_listLayout = nullptr;
    QLabel* m_empty = nullptr;
    QLabel* m_working = nullptr;
    QVector<QWidget*> m_widgets; // one per chat entry
    QWidget* m_approval = nullptr;
    QLabel* m_approvalText = nullptr;
    QPlainTextEdit* m_input = nullptr;
    QToolButton* m_sendButton = nullptr;
    QLabel* m_usage = nullptr;
    QTimer* m_flush = nullptr;
    QTimer* m_tick = nullptr; // the working line's seconds while the model is silent
    QVector<int> m_dirty;
    bool m_follow = true; // the list sticks to the newest message
    int m_slowHintMs = 30000;
    // Setup page
    QComboBox* m_setupProvider = nullptr;
    QLineEdit* m_setupKey = nullptr;
    QLineEdit* m_setupAddress = nullptr;
    class QFormLayout* m_setupForm = nullptr;
    QLabel* m_setupKeyHint = nullptr;
    QComboBox* m_setupModel = nullptr;
    QLabel* m_setupMessage = nullptr;
};
