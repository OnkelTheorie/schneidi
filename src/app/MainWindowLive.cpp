// MainWindow: live connection to an AI session (schneidi-cli / MCP, see cli/Live.h): state, control, indicator
#include "app/MainWindow.h"

#include "app/AiLog.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QScreen>
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

bool MainWindow::liveSave(const QString& path, QString* error)
{
    const QString target = path.isEmpty() ? m_projectPath : path;
    if (target.isEmpty()) {
        *error = "the project was never saved: give 'path' (a new .schneidi file), or the user saves it in the app";
        return false;
    }
    QDir().mkpath(QFileInfo(target).absolutePath());
    if (!saveTo(target)) {
        *error = "saving failed: " + target;
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

void MainWindow::liveChanged(const LiveChange& change)
{
    // Light up what changed in the shown timeline (never scroll there: arrows at the edge point to it)
    const Timeline& tl = m_project->timeline();
    const int shown = m_project->currentSequence();
    TimelineView::AiMarks marks;
    int first = -1; // where the step happened in the shown timeline
    const auto earliest = [&first](int frame) { first = first < 0 ? frame : std::min(first, frame); };
    for (const auto& [ids, out] : {std::pair{&change.added, &marks.added}, std::pair{&change.changed, &marks.changed}})
        for (int id : *ids)
            if (const Clip* c = TimelineOps::findClip(tl, id)) {
                *out << id;
                earliest(c->start);
            }
    for (const LiveChange::Removed& r : change.removed)
        if (r.sequence == shown) {
            marks.removed << TimelineView::AiMarks::Ghost{r.track, r.start, r.end};
            if (marks.added.isEmpty() && marks.changed.isEmpty()) earliest(r.start);
        }
    m_timeline->view()->markAiChanges(marks);

    // Log: the step, how many clips it made/changed/deleted and their names
    AiLog::Entry e;
    e.step = change.step;
    e.clips = marks.added + marks.changed;
    e.sequence = shown;
    e.frame = first;
    if (!change.error.isEmpty()) {
        e.state = AiLog::Entry::State::Failed;
        e.detail = T("fehlgeschlagen: %1").arg(change.error);
    } else {
        QStringList parts, names;
        if (!change.added.isEmpty()) parts << T("neu: %1").arg(change.added.size());
        if (!change.changed.isEmpty()) parts << T("geändert: %1").arg(change.changed.size());
        if (!change.removed.isEmpty()) parts << T("gelöscht: %1").arg(change.removed.size());
        const auto name = [&names](const QString& n) {
            if (!n.isEmpty() && !names.contains(n)) names << n;
        };
        for (const QVector<int>* ids : {&change.added, &change.changed})
            for (int id : *ids)
                for (const Sequence& q : m_project->sequences())
                    if (const Clip* c = TimelineOps::findClip(q.timeline, id)) {
                        name(m_project->clipName(*c));
                        break;
                    }
        for (const LiveChange::Removed& r : change.removed) name(r.name);
        QString list = names.mid(0, 3).join(", ");
        if (names.size() > 3) list = T("%1 (+%2 weitere)").arg(list).arg(names.size() - 3);
        e.detail = parts.join(" · ") + (parts.isEmpty() || list.isEmpty() ? "" : " – ") + list;
    }
    m_aiLog->add(e);
}

void MainWindow::liveActivity(const LiveActivity& activity)
{
    m_aiLog->activity(activity);
    updateLiveIndicator();
}

void MainWindow::showAiLog()
{
    if (!m_aiLogView) {
        m_aiLogView = new AiLogView(m_aiLog, this);
        m_aiLogView->setWindowFlags(Qt::Popup);
        connect(m_aiLogView, &AiLogView::activated, this, &MainWindow::aiLogActivated);
    }
    // Under the indicator (or the project name when the indicator is hidden), inside the screen
    QWidget* anchor = m_liveBox && m_liveBox->isVisible() ? static_cast<QWidget*>(m_liveLabel) : m_titleLabel;
    QPoint at = anchor->mapToGlobal(QPoint(anchor->width() / 2 - m_aiLogView->width() / 2, anchor->height() + 4));
    if (const QScreen* s = anchor->screen()) {
        const QRect free = s->availableGeometry();
        at.setX(std::clamp(at.x(), free.left(), std::max(free.left(), free.right() - m_aiLogView->width())));
    }
    m_aiLogView->move(at);
    m_aiLogView->show();
}

void MainWindow::aiLogActivated(int entryId)
{
    const AiLog::Entry* e = m_aiLog->entry(entryId);
    if (!e || (e->clips.isEmpty() && e->frame < 0)) return;
    // The user clicked: open the timeline of the step, select its clips that are still there, jump there and show
    // them (the view moves only because of this click)
    if (e->sequence >= 0 && e->sequence != m_project->currentSequence() && m_project->sequence(e->sequence))
        m_timeline->openSequence(e->sequence);
    const Timeline& tl = m_project->timeline();
    QSet<int> ids;
    int from = -1, to = -1;
    std::optional<TrackRef> track;
    for (int id : e->clips) {
        TrackRef ref;
        if (const Clip* c = TimelineOps::findClip(tl, id, &ref)) {
            ids.insert(id);
            if (from < 0 || c->start < from) {
                from = c->start;
                track = ref;
            }
            to = std::max(to, c->end());
        }
    }
    if (!ids.isEmpty()) {
        const QVector<int> linked = m_editor->withLinked(QVector<int>(ids.begin(), ids.end()));
        m_selection->set(QSet<int>(linked.begin(), linked.end()));
    }
    if (from < 0) from = to = e->frame;
    if (from < 0) return;
    jumpToFrame(from);
    m_timeline->view()->revealRange(from, std::max(from + 1, to), track);
}

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
    m_liveLabel = new QToolButton;
    m_liveLabel->setObjectName("live_label");
    m_liveLabel->setAutoRaise(true);
    m_liveLabel->setFocusPolicy(Qt::NoFocus);
    connect(m_liveLabel, &QToolButton::clicked, this, &MainWindow::showAiLog);
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
    // Bold while an AI is there (the button's own font, so its size follows the text)
    QFont font = m_liveLabel->font();
    font.setBold(m_aiSettings.liveAllowed && !m_liveRefused && active);
    m_liveLabel->setFont(font);
    using Mode = AiSettings::Indicator;
    const Mode mode = m_aiSettings.indicator;
    m_liveBox->setVisible(mode == Mode::Always || (mode == Mode::WhileActive && (active || m_liveRefused)));
    const QString dim = QString("QToolButton { color: %1; }").arg(Theme::textDim.name());
    const QString logTip = "\n" + T("Klick: KI-Protokoll");
    m_liveButton->setVisible(m_aiSettings.liveAllowed && (active || m_liveRefused));
    if (!m_aiSettings.liveAllowed) {
        m_liveLabel->setText(T("KI aus"));
        m_liveLabel->setStyleSheet(dim);
        m_liveLabel->setToolTip(T("KI-Verbindungen sind ausgeschaltet (Einstellungen › KI).") + logTip);
    } else if (m_liveRefused) {
        m_liveLabel->setText(T("KI getrennt"));
        m_liveLabel->setStyleSheet(dim);
        m_liveLabel->setToolTip(T("Eine KI kann dieses Projekt erst wieder bearbeiten, wenn du es erlaubst.") + logTip);
        m_liveButton->setText(T("Erlauben"));
    } else if (!active) { // "always": nothing connected right now
        m_liveLabel->setText("○ " + T("KI"));
        m_liveLabel->setStyleSheet(dim);
        m_liveLabel->setToolTip(T("Keine KI verbunden. Eine KI (schneidi-cli, MCP) kann das offene Projekt bearbeiten; "
                                  "jede Änderung ist ein Rückgängig-Schritt.")
                                + logTip);
    } else {
        // Work outside the app (analyses, render …): what and how far, else just connected
        if (const AiLog::Entry* job = m_aiLog->runningJob())
            m_liveLabel->setText("● " + (job->percent >= 0 ? T("KI %1 … %2 %").arg(AiLog::jobText(job->tool)).arg(job->percent)
                                                           : T("KI %1 …").arg(AiLog::jobText(job->tool))));
        else
            m_liveLabel->setText("● " + T("KI verbunden"));
        m_liveLabel->setStyleSheet(QString("QToolButton { color: %1; }").arg(m_aiSettings.color.name()));
        m_liveLabel->setToolTip(T("%1 bearbeitet dieses Projekt. Jede Änderung ist ein Rückgängig-Schritt.")
                                    .arg(m_liveClient.isEmpty() ? QString("schneidi-cli") : m_liveClient)
                                + logTip);
        m_liveButton->setText(T("Trennen"));
    }
}
