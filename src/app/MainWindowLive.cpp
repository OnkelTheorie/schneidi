// MainWindow: live connection to an AI session (schneidi-cli / MCP, see cli/Live.h): state, control, indicator
#include "app/MainWindow.h"

#include "app/Theme.h"
#include "core/I18n.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QToolButton>

int MainWindow::livePlayhead() const { return m_timeline->view()->playhead(); }

bool MainWindow::livePlaying() const { return m_engine->timelinePositions() && m_engine->speed() != 0.0; }

// Like a click into the ruler: playback keeps running with the seek fade. Pause + play reopened the audio
// device on every seek (gap + click), and quick seeks did not count as dragging.
void MainWindow::liveSeek(int frame)
{
    if (frame < 0) return;
    if (livePlaying()) m_engine->seek(frame);
    else jumpToFrame(frame);
}

void MainWindow::livePlay(bool play)
{
    if (!play) {
        m_engine->pause();
        return;
    }
    if (m_engine->mode() == Engine::Mode::Source) m_engine->showTimeline(livePlayhead());
    m_engine->play();
}

bool MainWindow::liveSave(QString* error)
{
    if (m_projectPath.isEmpty()) {
        *error = "the project was never saved: the user has to save it once in the app (File > Save)";
        return false;
    }
    if (!saveTo(m_projectPath)) {
        *error = "saving failed: " + m_projectPath;
        return false;
    }
    return true;
}

void MainWindow::liveGenerateProxies(const QStringList& files) { m_engine->proxies()->generate(files); }

QJsonObject MainWindow::liveProxyState(const QString& file) const
{
    const ProxyManager* pm = m_engine->proxies();
    if (pm->hasProxy(file)) return {{"state", "ready"}};
    if (pm->isPending(file)) return {{"state", "pending"}, {"percent", std::max(0, pm->progressOf(file))}};
    return {{"state", "none"}};
}

void MainWindow::liveConnectionChanged(bool connected, const QString& client)
{
    m_liveConnected = connected;
    if (connected) {
        m_liveClient = client;
        m_liveLinger.stop();
    } else {
        m_liveLinger.start(); // one-shot commands connect and leave right away: stay visible a moment
    }
    updateLiveIndicator();
}

void MainWindow::liveClipsChanged(const QVector<int>& clipIds) { m_timeline->view()->flashClips(clipIds); }

void MainWindow::setLiveControls(const std::function<void(const QString&)>& disconnect, const std::function<void()>& allow)
{
    m_liveDisconnect = disconnect;
    m_liveAllow = allow;
    if (!m_aiSettings.liveAllowed && m_liveDisconnect) m_liveDisconnect(liveOffReason());
    updateLiveIndicator();
}

QString MainWindow::liveOffReason()
{
    return "the user turned AI connections off in the schneidi app (Settings > AI); only they can turn them on again";
}

QWidget* MainWindow::buildLiveIndicator()
{
    // Like "Claude is using this tab" in a browser: visible while an AI works on the project
    m_liveBox = new QWidget;
    m_liveBox->setObjectName("live_indicator");
    auto* lay = new QHBoxLayout(m_liveBox);
    lay->setContentsMargins(12, 0, 0, 0);
    lay->setSpacing(6);
    m_liveLabel = new QLabel;
    m_liveLabel->setObjectName("live_label");
    m_liveButton = new QToolButton;
    m_liveButton->setObjectName("live_button");
    lay->addWidget(m_liveLabel);
    lay->addWidget(m_liveButton);
    m_liveLinger.setSingleShot(true);
    m_liveLinger.setInterval(2500);
    connect(&m_liveLinger, &QTimer::timeout, this, &MainWindow::updateLiveIndicator);
    connect(m_liveButton, &QToolButton::clicked, this, [this] {
        if (m_liveRefused) {
            m_liveRefused = false;
            if (m_liveAllow) m_liveAllow();
        } else {
            m_liveRefused = true;
            m_liveLinger.stop();
            if (m_liveDisconnect) m_liveDisconnect({});
        }
        updateLiveIndicator();
    });
    updateLiveIndicator();
    return m_liveBox;
}

void MainWindow::updateLiveIndicator()
{
    if (!m_liveBox) return;
    const bool active = m_liveConnected || m_liveLinger.isActive();
    using Mode = AiSettings::Indicator;
    const Mode mode = m_aiSettings.indicator;
    m_liveBox->setVisible(mode == Mode::Always || (mode == Mode::WhileActive && (active || m_liveRefused)));
    const QString dim = QString("color: %1;").arg(Theme::textDim.name());
    m_liveButton->setVisible(m_aiSettings.liveAllowed && (active || m_liveRefused));
    if (!m_aiSettings.liveAllowed) {
        m_liveLabel->setText(T("KI aus"));
        m_liveLabel->setStyleSheet(dim);
        m_liveLabel->setToolTip(T("KI-Verbindungen sind ausgeschaltet (Einstellungen › KI)."));
    } else if (m_liveRefused) {
        m_liveLabel->setText(T("KI getrennt"));
        m_liveLabel->setStyleSheet(dim);
        m_liveLabel->setToolTip(T("Eine KI kann dieses Projekt erst wieder bearbeiten, wenn du es erlaubst."));
        m_liveButton->setText(T("Erlauben"));
    } else if (!active) { // "always": nothing connected right now
        m_liveLabel->setText("○ " + T("KI"));
        m_liveLabel->setStyleSheet(dim);
        m_liveLabel->setToolTip(T("Keine KI verbunden. Eine KI (schneidi-cli, MCP) kann das offene Projekt bearbeiten; "
                                  "jede Änderung ist ein Rückgängig-Schritt."));
    } else {
        m_liveLabel->setText("● " + T("KI verbunden"));
        m_liveLabel->setStyleSheet(QString("color: %1; font-weight: 600;").arg(m_aiSettings.color.name()));
        m_liveLabel->setToolTip(T("%1 bearbeitet dieses Projekt. Jede Änderung ist ein Rückgängig-Schritt.")
                                    .arg(m_liveClient.isEmpty() ? QString("schneidi-cli") : m_liveClient));
        m_liveButton->setText(T("Trennen"));
    }
}
