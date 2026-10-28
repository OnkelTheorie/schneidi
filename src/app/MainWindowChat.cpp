// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the AI chat panel (app/AiChat.h, app/AiChatPanel.h): placing it beside the pages, the provider and its
// key, the privacy notice, and the review mode (the changes of a request become one undo step or go back)
#include "app/MainWindow.h"

#include "app/AiChat.h"
#include "app/AiChatPanel.h"
#include "app/AiKeys.h"
#include "app/McpClient.h"
#include "core/I18n.h"
#include "core/Project.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <algorithm>

namespace {

// The model answers in the language of the interface
QString languageName(const QString& code)
{
    static const QHash<QString, QString> names{{"de", "German"}, {"en", "English"}, {"es", "Spanish"},
                                               {"fr", "French"}, {"pl", "Polish"},  {"ru", "Russian"}};
    if (names.contains(code)) return names.value(code);
    for (const I18n::Language& l : I18n::languages())
        if (l.code == code) return l.name;
    return "English";
}

constexpr int kChatWidth = 380;

} // namespace

MainWindow::~MainWindow()
{
    // The project is a child QObject and dies only in ~QWidget, after this destructor: clearing its undo stack there
    // emits indexChanged into our slots (chatUndoChanged …) on a half-destroyed window (UBSan "downcast", clang)
    m_project->undoStack()->blockSignals(true);
    m_project->blockSignals(true);
}

void MainWindow::buildChat()
{
    m_aiKeys = std::make_unique<Ai::Keys>();
    m_chat = new AiChat(this);
    m_chat->setLanguage(languageName(I18n::language()));
    m_chat->setCompact(m_aiSettings.compact);
    m_chatPanel = new AiChatPanel(m_chat);
    m_chatPanel->setKeyHint([this](const QString& provider) { return chatKeyHint(provider); });
    m_chatPanel->setSender([this](const QString& text) { return sendChat(text); });
    m_chatSplit = new QSplitter(Qt::Horizontal);
    m_chatSplit->setObjectName("aiChatSplit");
    m_chatSplit->addWidget(m_pages);
    m_chatSplit->setChildrenCollapsible(false);
    connect(m_chatSplit, &QSplitter::splitterMoved, this, [this] {
        if (m_autosaveDisabled || m_chatPanel->isHidden()) return; // test runs leave the user's layout alone
        QSettings().setValue("layout/aiChatWidth", m_chatPanel->width());
    });
    placeChat(QSettings().value("panels/aiChatSide").toString() == "left");
}

void MainWindow::setupChat()
{
    connect(m_chatPanel, &AiChatPanel::settingsRequested, this, [this] { settingsDialog(1); });
    connect(m_chatPanel, &AiChatPanel::sideRequested, this, [this](bool left) {
        placeChat(left);
        if (!m_autosaveDisabled) QSettings().setValue("panels/aiChatSide", left ? "left" : "right");
    });
    connect(m_chatPanel, &AiChatPanel::hideRequested, this, [this] { showAiChat(false); });
    connect(m_chatToggle, &QToolButton::toggled, this, [this](bool on) {
        if (on) QTimer::singleShot(0, this, &MainWindow::applyChatWidth); // after the splitter saw it appear
    });
    connect(m_chatPanel, &AiChatPanel::approved, this, &MainWindow::acceptChatChanges);
    connect(m_chatPanel, &AiChatPanel::discarded, this, &MainWindow::discardChatChanges);
    connect(m_chatPanel, &AiChatPanel::setupDone, this, [this](const AiChatPanel::SetupChoice& c) {
        AiSettings s = m_aiSettings;
        s.chatProvider = c.provider;
        s.chatAddress = c.address;
        s.chatModels.insert(c.provider, c.model);
        if (!c.key.isEmpty()) {
            QString error;
            if (!m_aiKeys->store(c.provider, c.key, &error)) {
                m_chatPanel->showSetup(true, c.provider, T("Schlüssel nicht gespeichert: %1").arg(error));
                return;
            }
        }
        setAiSettings(s); // also updates the panel
    });
    connect(m_chat, &AiChat::turnFinished, this, &MainWindow::chatTurnFinished);
    connect(m_chat, &AiChat::cleared, this, [this] {
        // A new chat while the bar waits: the changes stay (one step each), nothing is thrown away unasked
        if (m_review.pending) m_chatPanel->hideApproval();
        m_review = {};
        m_chatSeen.reset();
    });
    connect(m_project->undoStack(), &QUndoStack::indexChanged, this, &MainWindow::chatUndoChanged);
    updateChatPanel();
}

void MainWindow::setAiChatTools(const QStringList& command, const QHash<QString, QString>& environment)
{
    m_chat->setTools(command, environment);
}

void MainWindow::setAiChatOffline(bool on) { m_chat->setOffline(on); }

void MainWindow::showAiChat(bool on)
{
    if (m_chatToggle->isChecked() != on) m_chatToggle->setChecked(on); // shows/hides the panel, remembered
}

void MainWindow::placeChat(bool left)
{
    m_chatSplit->insertWidget(left ? 0 : 1, m_chatPanel);
    m_chatSplit->setStretchFactor(m_chatSplit->indexOf(m_pages), 1);
    m_chatSplit->setStretchFactor(m_chatSplit->indexOf(m_chatPanel), 0);
    applyChatWidth();
    m_chatPanel->setDockedLeft(left);
    // The button sits on the panel's side of the top bar, next to Effects or Transcript (as in buildTopBar)
    if (m_topBarLayout && m_chatToggle) {
        m_topBarLayout->removeWidget(m_chatToggle);
        const int next = m_topBarLayout->indexOf(left ? m_effectsToggle : m_transcriptToggle);
        m_topBarLayout->insertWidget(left ? next + 1 : next, m_chatToggle);
    }
}

void MainWindow::applyChatWidth()
{
    // The remembered width (a panel shown again or moved gets it back, the pages the rest)
    const int width = std::max(260, QSettings().value("layout/aiChatWidth", kChatWidth).toInt());
    const int total = std::max(m_chatSplit->width(), width + 600);
    QList<int> sizes;
    for (int i = 0; i < m_chatSplit->count(); ++i) sizes << (m_chatSplit->widget(i) == m_chatPanel ? width : total - width);
    m_chatSplit->setSizes(sizes);
}

QString MainWindow::chatKeyHint(const QString& provider)
{
    for (const QString& var : Ai::keyVariables(provider))
        if (!qEnvironmentVariable(var.toLatin1().constData()).trimmed().isEmpty())
            return T("Schlüssel aus der Umgebungsvariable %1").arg(var);
    const Ai::Company company = Ai::company();
    if (company.found() && !company.key.isEmpty() && (company.provider.isEmpty() || company.provider == provider))
        return T("Schlüssel aus der Firmen-Datei");
    if (m_aiKeys->hasStored(provider)) return T("Schlüssel gespeichert (%1)").arg(m_aiKeys->storeName());
    return {};
}

void MainWindow::updateChatPanel()
{
    if (!m_chatPanel) return;
    const Ai::Company company = Ai::company();
    const Ai::Choice c = Ai::choose(m_aiSettings.chatProvider, m_aiSettings.chatAddress, m_aiSettings.chatModels, company);
    const Ai::Preset* p = Ai::preset(c.provider);
    m_chatPanel->setProviderText(p ? c.model : QString(),
                                 p ? QString("%1 · %2\n%3").arg(Ai::presetName(c.provider), c.model, c.address) : QString());
    // Setup when nothing is chosen yet, or the provider needs a key there is none of
    QString message;
    bool setup = !p;
    if (p && p->needsKey && chatKeyHint(c.provider).isEmpty()) {
        setup = true;
        message = T("Für %1 fehlt noch der Schlüssel.").arg(Ai::presetName(c.provider));
    } else if (p && c.address.isEmpty()) {
        setup = true;
        message = T("Bitte die Adresse des Servers angeben.");
    }
    m_chatPanel->showSetup(setup, c.provider.isEmpty() ? QString("claude") : c.provider, message);
}

bool MainWindow::chatEndpoint(Ai::Endpoint& e, QString* problem)
{
    const Ai::Company company = Ai::company();
    const Ai::Choice c = Ai::choose(m_aiSettings.chatProvider, m_aiSettings.chatAddress, m_aiSettings.chatModels, company);
    const Ai::Preset* p = Ai::preset(c.provider);
    if (!p) {
        *problem = T("Noch kein Anbieter eingerichtet.");
        return false;
    }
    e.protocol = p->protocol;
    e.provider = c.provider;
    e.address = c.address;
    e.model = c.model;
    if (e.address.isEmpty() || e.model.isEmpty()) {
        *problem = T("Adresse oder Modell fehlt (Einstellungen › KI).");
        return false;
    }
    e.key = m_aiKeys->resolve(c.provider, company).key;
    if (p->needsKey && e.key.isEmpty()) {
        *problem = T("Für %1 fehlt der Schlüssel (Einstellungen › KI).").arg(Ai::presetName(c.provider));
        return false;
    }
    return true;
}

bool MainWindow::sendChat(const QString& text)
{
    if (m_chat->busy() || m_review.pending) return false;
    if (m_chat->offline()) return m_chat->send(text, {}, false); // says why nothing goes out
    Ai::Endpoint e;
    QString problem;
    if (!chatEndpoint(e, &problem)) {
        m_chat->addError(problem);
        updateChatPanel();
        return false;
    }
    // Before the first message to a cloud provider: what goes there (local servers keep everything here)
    const QString host = QUrl(e.address).host();
    const QString accepted = e.provider + "|" + host;
    QStringList known = QSettings().value("ai/chat/privacyAccepted").toStringList();
    if (!Ai::isLocal(e.address) && !known.contains(accepted)) {
        QMessageBox box(QMessageBox::Information, T("KI-Chat"),
                        T("Deine Nachrichten gehen an %1 (%2).").arg(Ai::presetName(e.provider), host),
                        QMessageBox::Ok | QMessageBox::Cancel, this);
        box.setInformativeText(T("Mit ihnen geht, was die KI für ihre Arbeit braucht: Inhalt des Projekts (Clips, "
                                 "Dateinamen, Titel, Untertitel), Ergebnisse ihrer Werkzeuge, Transkripte und "
                                 "Standbilder aus deinen Videos. Der Anbieter verarbeitet sie nach seinen eigenen "
                                 "Regeln. Der Chat selbst bleibt nur hier im Speicher.\n\nDiese Frage kommt einmal je "
                                 "Anbieter."));
        box.setObjectName("ai_privacy");
        if (box.exec() != QMessageBox::Ok) return false;
        known << accepted;
        if (!m_autosaveDisabled) QSettings().setValue("ai/chat/privacyAccepted", known);
    }
    if (const QString note = chatChangesNote(); !note.isEmpty()) m_chat->addNote(note);
    const bool review = m_aiSettings.approval == AiSettings::Approval::Review;
    if (review) m_review = {true, m_project->undoStack()->index(), false, false};
    if (!m_chat->send(text, e, review)) {
        m_review = {};
        return false;
    }
    return true;
}

void MainWindow::liveCommandRunning(bool running, bool ownChat)
{
    m_chatCommand = running && ownChat;
    m_liveCommand = running;
}

void MainWindow::chatUndoChanged()
{
    // Only the own chat's commands belong to the request; anything else (the user, another AI, opening a project)
    // and the request's changes stay single steps – nothing of the user is ever thrown away
    if (!m_review.active || m_chatCommand) return;
    m_review.broken = true;
    if (!m_review.pending) return;
    m_review = {};
    m_chatPanel->hideApproval();
    m_chat->addNotice(T("Du hast weiterbearbeitet: die Änderungen der KI bleiben als einzelne Rückgängig-Schritte."));
}

void MainWindow::rememberChatState()
{
    m_chatSeen = m_project->timeline();
    m_chatSeenSequence = m_project->currentSequence();
    m_chatSeenProject = m_projectPath;
}

namespace {

// A clip as the chat's AI knows it from `info`, with the values a user changes by hand (place, length, source range,
// volume, fades, speed); by id
QHash<int, QJsonObject> chatClips(const Project& p, const Timeline& tl)
{
    QHash<int, QJsonObject> out;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < tl.tracks(k).size(); ++i)
            for (const Clip& c : tl.tracks(k)[i].clips) {
                QJsonObject o{{"id", c.id}, {"track", trackShortName({k, i})}, {"name", p.clipName(c)},
                              {"start", c.start}, {"end", c.end()}};
                if (c.kind == ClipKind::Media) {
                    o["src_in"] = c.in;
                    o["src_out"] = c.out + 1;
                }
                if (!c.enabled) o["enabled"] = false;
                if (k == TrackKind::Audio && c.volumeDb != 0) o["volume_db"] = c.volumeDb;
                if (c.speed != 1.0) o["speed"] = c.speed;
                if (c.fadeIn) o["fade_in"] = c.fadeIn;
                if (c.fadeOut) o["fade_out"] = c.fadeOut;
                out.insert(c.id, o);
            }
    return out;
}

const Clip* findClip(const Timeline& tl, int id)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            for (const Clip& c : t.clips)
                if (c.id == id) return &c;
    return nullptr;
}

} // namespace

QString MainWindow::chatChangesNote() const
{
    if (!m_chatSeen || m_chatSeenProject != m_projectPath) return {};
    if (m_chatSeenSequence != m_project->currentSequence())
        return "since your last answer the user opened another timeline; look at `info` before you edit";
    const Timeline& now = m_project->timeline();
    const QHash<int, QJsonObject> before = chatClips(*m_project, *m_chatSeen), after = chatClips(*m_project, now);
    QJsonArray added, changed, removed;
    QList<int> ids = after.keys();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        const QJsonObject& o = after[id];
        if (!before.contains(id)) {
            added << o;
            continue;
        }
        const QJsonObject& old = before[id];
        QJsonObject was; // the values before, of those that changed
        for (auto it = o.begin(); it != o.end(); ++it)
            if (old.value(it.key()) != it.value()) was[it.key()] = old.contains(it.key()) ? old[it.key()] : QJsonValue();
        for (auto it = old.begin(); it != old.end(); ++it)
            if (!o.contains(it.key())) was[it.key()] = it.value();
        const Clip* a = findClip(*m_chatSeen, id);
        const Clip* b = findClip(now, id);
        const bool other = a && b && !(*a == *b) && was.isEmpty(); // effects, keyframes, transform …
        if (was.isEmpty() && !other) continue;
        QJsonObject c = o;
        if (!was.isEmpty()) c["was"] = was;
        if (other) c["other"] = "effects, keyframes or transform changed";
        changed << c;
    }
    QList<int> gone = before.keys();
    std::sort(gone.begin(), gone.end());
    for (int id : gone)
        if (!after.contains(id))
            removed << QJsonObject{{"id", id}, {"track", before[id]["track"]}, {"name", before[id]["name"]}};
    if (added.isEmpty() && changed.isEmpty() && removed.isEmpty()) return {};
    QJsonObject changes;
    if (!added.isEmpty()) changes["added"] = added;
    if (!changed.isEmpty()) changes["changed"] = changed;
    if (!removed.isEmpty()) changes["removed"] = removed;
    QString list = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));
    if (list.size() > 6000) // a big change (a new format touches every clip): the counts, `info` has the rest
        list = QString("%1 clip(s) added, %2 changed, %3 removed - look at `info` before you edit")
                   .arg(added.size()).arg(changed.size()).arg(removed.size());
    return "since your last answer the user changed clips of the open timeline by hand (frames in the project's "
           "rate, `was` = the values before): " + list + ". Build on these changes: keep them when you rework "
           "something.";
}

void MainWindow::chatTurnFinished()
{
    rememberChatState();
    if (!m_review.active) return;
    const int index = m_project->undoStack()->index();
    if (m_review.broken || index <= m_review.start) {
        if (m_review.broken)
            m_chat->addNotice(T("Während der Anfrage wurde das Projekt auch anders bearbeitet: die Änderungen der KI "
                                "bleiben als einzelne Rückgängig-Schritte."));
        m_review = {};
        return;
    }
    m_review.pending = true;
    showAiChat(true);
    m_chatPanel->showApproval(T("Die KI hat das Projekt geändert – zu sehen in Timeline und Vorschau. Übernehmen oder "
                                "verwerfen? Bis dahin wartet sie."));
}

void MainWindow::acceptChatChanges()
{
    if (!m_review.pending) return;
    const int start = m_review.start;
    m_review = {}; // before the undo stack moves
    QString request = m_chat->request().simplified();
    if (request.size() > 40) request = request.left(39) + "…";
    m_project->combineSteps(start, T("KI-Chat: %1").arg(request));
    m_chatPanel->hideApproval();
    m_chat->addNotice(T("Übernommen (ein Rückgängig-Schritt)."));
}

void MainWindow::discardChatChanges()
{
    if (!m_review.pending) return;
    const int start = m_review.start;
    m_review = {};
    m_project->undoStack()->setIndex(start);
    rememberChatState(); // the note below says it; the removed changes are not the user's edits
    m_chatPanel->hideApproval();
    m_chat->addNotice(T("Verworfen: das Projekt ist wieder wie vor der Anfrage."));
    m_chat->addNote("the user discarded all changes of your last request; the project is back as it was before it");
}
