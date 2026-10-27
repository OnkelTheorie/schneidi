#include "app/AiSettingsPage.h"

#include "app/AiClient.h"
#include "app/Theme.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

} // namespace

AiSettingsPage::AiSettingsPage(const AiSettings& s, QWidget* parent) : QWidget(parent), m_initial(s), m_color(s.color)
{
    m_sections = new QVBoxLayout(this);
    m_sections->setContentsMargins(12, 12, 12, 12);
    m_sections->setSpacing(10);
    m_sections->addStretch(1);

    // Connection: whether AIs may work on the open project at all (the indicator's "Disconnect" is for this session)
    QFormLayout* f = formOf(addSection(T("Verbindung")));
    m_liveAllowed = new QCheckBox(T("KIs dürfen das offene Projekt bearbeiten (schneidi-cli, MCP)"));
    m_liveAllowed->setChecked(s.liveAllowed);
    f->addRow(m_liveAllowed);
    f->addRow(hint(T("Aus: eine KI erreicht die App nicht mehr und bearbeitet nur Projektdateien, die hier nicht offen "
                     "sind. „Trennen“ oben neben dem Projektnamen gilt nur bis zum Neustart.")));
    m_release = new QSpinBox;
    m_release->setObjectName("ai_release");
    m_release->setRange(0, AiSettings::kMaxReleaseMinutes);
    m_release->setSuffix(T(" min"));
    m_release->setSpecialValueText(T("Nie"));
    m_release->setValue(s.releaseMinutes);
    f->addRow(T("Freigeben nach"), m_release);
    f->addRow(hint(T("Eine KI, die so lange nichts schickt, gibt die App frei: Eine andere darf dann ran, die erste "
                     "holt sie sich mit ihrer nächsten Anfrage zurück, wenn sie frei ist. Laufende Arbeit (ein Render) "
                     "zählt nicht als Pause. Nie = sie behält die App, bis sie sich trennt.")));
    connect(m_liveAllowed, &QCheckBox::toggled, this, &AiSettingsPage::updateEnabled);

    // What an AI changes lights up in the timeline (never scrolls there)
    f = formOf(addSection(T("Änderungen in der Timeline")));
    m_highlight = new QCheckBox(T("Was eine KI ändert, kurz aufleuchten lassen"));
    m_highlight->setChecked(s.highlight);
    f->addRow(m_highlight);
    auto* colorRow = new QHBoxLayout;
    m_colorButton = new QToolButton;
    m_colorButton->setObjectName("ai_color");
    m_colorButton->setFixedSize(36, 18);
    m_colorButton->setToolTip(T("Farbe wählen"));
    m_colorButton->setFocusPolicy(Qt::TabFocus);
    connect(m_colorButton, &QToolButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_color, this, T("Farbe der KI-Änderungen"));
        if (c.isValid()) setColor(c);
    });
    m_colorReset = new QToolButton;
    m_colorReset->setText(T("Zurücksetzen"));
    connect(m_colorReset, &QToolButton::clicked, this, [this] { setColor(AiSettings::defaultColor()); });
    colorRow->addWidget(m_colorButton);
    colorRow->addWidget(m_colorReset);
    colorRow->addStretch(1);
    f->addRow(T("Farbe"), colorRow);
    m_duration = new QDoubleSpinBox;
    m_duration->setRange(AiSettings::kMinHighlightMs / 1000.0, AiSettings::kMaxHighlightMs / 1000.0);
    m_duration->setDecimals(1);
    m_duration->setSingleStep(0.5);
    m_duration->setSuffix(T(" s"));
    m_duration->setValue(s.highlightMs / 1000.0);
    f->addRow(T("Dauer"), m_duration);
    m_distinguish = new QCheckBox(T("Neue und bearbeitete Clips unterscheiden"));
    m_distinguish->setChecked(s.distinguish);
    f->addRow(m_distinguish);
    f->addRow(hint(T("Neu = gefüllt, bearbeitet = Rand, gelöscht = gestrichelter Umriss. Liegt eine Änderung "
                     "außerhalb der Sicht, zeigt ein Pfeil am Rand dorthin; die Timeline scrollt nie von selbst.")));
    connect(m_highlight, &QCheckBox::toggled, this, &AiSettingsPage::updateEnabled);

    // The indicator next to the project name
    f = formOf(addSection(T("Anzeige oben")));
    m_indicator = new QComboBox;
    m_indicator->addItem(T("Solange eine KI verbunden ist"), int(AiSettings::Indicator::WhileActive));
    m_indicator->addItem(T("Immer"), int(AiSettings::Indicator::Always));
    m_indicator->addItem(T("Nie"), int(AiSettings::Indicator::Never));
    m_indicator->setCurrentIndex(std::max(0, m_indicator->findData(int(s.indicator))));
    f->addRow(T("KI-Anzeige"), m_indicator);

    // The AI log behind the indicator
    f = formOf(addSection(T("Protokoll")));
    m_logLength = new QSpinBox;
    m_logLength->setObjectName("ai_log_length");
    m_logLength->setRange(0, AiSettings::kMaxLogLength);
    m_logLength->setSingleStep(50);
    m_logLength->setSpecialValueText(T("Aus"));
    m_logLength->setValue(s.logLength);
    f->addRow(T("Einträge behalten"), m_logLength);
    f->addRow(hint(T("Klick auf die KI-Anzeige oben (oder Arbeitsbereich › KI-Protokoll) zeigt, was eine KI getan hat: "
                     "Schritte, Clips, Analysen. Nur bis zum Beenden, nicht im Projekt.")));

    setColor(m_color);
    updateEnabled();
}

QGroupBox* AiSettingsPage::addSection(const QString& title, int index)
{
    auto* box = new QGroupBox(title);
    auto* form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    const int stretch = m_sections->count() - 1; // the stretch stays last
    m_sections->insertWidget(index < 0 || index > stretch ? stretch : index, box);
    return box;
}

QFormLayout* AiSettingsPage::formOf(QGroupBox* section) { return qobject_cast<QFormLayout*>(section->layout()); }

void AiSettingsPage::setColor(const QColor& c)
{
    m_color = c;
    m_colorButton->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                     .arg(c.name(), Theme::border.name()));
    m_colorReset->setEnabled(c != AiSettings::defaultColor());
}

void AiSettingsPage::updateEnabled()
{
    m_release->setEnabled(m_liveAllowed->isChecked());
    const bool on = m_highlight->isChecked();
    for (QWidget* w : {static_cast<QWidget*>(m_colorButton), static_cast<QWidget*>(m_duration),
                       static_cast<QWidget*>(m_distinguish)})
        w->setEnabled(on);
    m_colorReset->setEnabled(on && m_color != AiSettings::defaultColor());
}

AiSettings AiSettingsPage::settings() const
{
    AiSettings s = m_initial;
    s.liveAllowed = m_liveAllowed->isChecked();
    s.releaseMinutes = m_release->value();
    s.highlight = m_highlight->isChecked();
    s.color = m_color;
    s.highlightMs = int(std::lround(m_duration->value() * 1000));
    s.distinguish = m_distinguish->isChecked();
    s.indicator = AiSettings::Indicator(m_indicator->currentData().toInt());
    s.logLength = m_logLength->value();
    if (m_chatProvider) {
        const QString id = chatProvider();
        // Not set up yet and nothing touched here: stays not set up (OK for another reason chooses no provider)
        const bool untouched = m_initial.chatProvider.isEmpty() && id == m_startProvider
                               && m_chatModel->currentText().trimmed() == m_startModel
                               && m_chatAddress->text().trimmed().isEmpty() && m_chatKey->text().trimmed().isEmpty();
        if (m_chatProvider->isEnabled() && !untouched) s.chatProvider = id; // locked: the company file decides
        if (m_chatAddress->isEnabled()) s.chatAddress = m_chatAddress->text().trimmed();
        s.chatModels = m_models;
        s.chatModels.insert(id, m_chatModel->currentText().trimmed());
        for (auto it = s.chatModels.begin(); it != s.chatModels.end();) { // only what differs from the default
            const Ai::Preset* q = Ai::preset(it.key());
            const bool plain = it.value().isEmpty() || (q && it.value() == q->model && !m_company.found());
            it = plain ? s.chatModels.erase(it) : std::next(it);
        }
        s.approval = AiSettings::Approval(m_approval->currentData().toInt());
        s.compact = Ai::Compact(m_compact->currentData().toInt());
    }
    return s;
}

void AiSettingsPage::setClaudeSetup(const ClaudeSetup& setup)
{
    m_claudeSetup = setup;
    if (!setup || m_claudeConnect) return;
    // Right below "Connection" (a chat section of its own goes first)
    QGroupBox* box = addSection(T("Claude verbinden"), 1);
    QFormLayout* f = formOf(box);
    f->addRow(hint(T("Trägt schneidi in Claude Code und Claude Desktop ein (MCP-Server und Anleitung), dann kann "
                     "Claude das offene Projekt bearbeiten. Ändert nur die Einstellungen von Claude.")));
    auto* row = new QHBoxLayout;
    m_claudeConnect = new QPushButton(T("Mit Claude verbinden…"));
    m_claudeConnect->setObjectName("claude_connect");
    m_claudeRemove = new QPushButton(T("Entfernen"));
    m_claudeRemove->setObjectName("claude_remove");
    m_claudeRemove->setToolTip(T("schneidi wieder aus Claude Code und Claude Desktop austragen"));
    row->addWidget(m_claudeConnect);
    row->addWidget(m_claudeRemove);
    row->addStretch(1);
    f->addRow(row);
    m_claudeResult = new QLabel;
    m_claudeResult->setObjectName("claude_result");
    m_claudeResult->setWordWrap(true);
    m_claudeResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_claudeResult->hide();
    f->addRow(m_claudeResult);
    connect(m_claudeConnect, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, T("Mit Claude verbinden"),
                                  T("schneidi in Claude Code und Claude Desktop eintragen? Claude Desktop braucht "
                                    "danach einen Neustart."))
            == QMessageBox::Yes)
            runClaudeSetup(false);
    });
    connect(m_claudeRemove, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, T("Mit Claude verbinden"),
                                  T("schneidi aus Claude Code und Claude Desktop austragen?"))
            == QMessageBox::Yes)
            runClaudeSetup(true);
    });
}

void AiSettingsPage::runClaudeSetup(bool remove)
{
    if (!m_claudeSetup || !m_claudeConnect->isEnabled()) return;
    m_claudeConnect->setEnabled(false);
    m_claudeRemove->setEnabled(false);
    m_claudeResult->setText(remove ? T("Entferne…") : T("Verbinde…"));
    m_claudeResult->show();
    // Claude Code's own command may take a while: not in the GUI thread
    QPointer<AiSettingsPage> page(this);
    QThread* worker = QThread::create([page, setup = m_claudeSetup, remove] {
        const QJsonObject result = setup(remove);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [page, result, remove] {
            if (!page) return;
            page->m_claudeResult->setText(claudeResultText(result, remove));
            page->m_claudeConnect->setEnabled(true);
            page->m_claudeRemove->setEnabled(true);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

QString AiSettingsPage::claudeResultText(const QJsonObject& result, bool remove)
{
    QStringList lines;
    for (const auto& [key, name] : {std::pair{"claude_code", "Claude Code"}, std::pair{"claude_desktop", "Claude Desktop"}}) {
        const QJsonObject r = result.value(key).toObject();
        if (r.isEmpty()) continue;
        const QString state = r.value("state").toString();
        const QString word = state == "done"      ? (remove ? T("entfernt") : T("eingerichtet"))
                             : state == "partly"  ? T("teilweise eingerichtet")
                             : state == "skipped" ? T("nicht gefunden, übersprungen")
                                                  : T("fehlgeschlagen");
        QString line = T("%1: %2").arg(name, word);
        // The details come from the setup in English (paths, Claude's own messages)
        if (const QString msg = r.value("message").toString(); !msg.isEmpty() && state != "skipped")
            line += " (" + msg + ")";
        lines << line;
    }
    return lines.isEmpty() ? T("fehlgeschlagen") : lines.join("\n");
}

// ---------- AI chat ----------

void AiSettingsPage::addChatSection(Ai::Keys* keys)
{
    if (m_chatProvider || !keys) return;
    m_keys = keys;
    m_company = Ai::company();
    m_models = m_initial.chatModels;
    m_client = new AiClient(this);
    QGroupBox* box = addSection(T("KI-Chat"), 0);
    box->setObjectName("ai_chat_section");
    QFormLayout* f = formOf(box);
    f->addRow(hint(T("Im Panel „KI“ bearbeitet eine KI das offene Projekt auf Zuruf, mit denselben Werkzeugen wie jede "
                     "andere KI. Jede Änderung lässt sich rückgängig machen.")));
    m_chatProvider = new QComboBox;
    m_chatProvider->setObjectName("ai_chat_provider");
    for (const Ai::Preset& p : Ai::presets()) m_chatProvider->addItem(T(p.name), p.id);
    const Ai::Choice choice = Ai::choose(m_initial.chatProvider, m_initial.chatAddress, m_initial.chatModels, m_company);
    m_chatProvider->setCurrentIndex(
        std::max(0, m_chatProvider->findData(choice.provider.isEmpty() ? QString("claude") : choice.provider)));
    f->addRow(T("Anbieter"), m_chatProvider);
    m_chatAddress = new QLineEdit;
    m_chatAddress->setObjectName("ai_chat_address");
    m_chatAddress->setClearButtonEnabled(true);
    f->addRow(T("Adresse"), m_chatAddress);
    auto* modelRow = new QHBoxLayout;
    m_chatModel = new QComboBox;
    m_chatModel->setObjectName("ai_chat_model");
    m_chatModel->setEditable(true);
    m_chatModel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* load = new QPushButton(T("Modelle laden"));
    load->setObjectName("ai_chat_models");
    load->setToolTip(T("Die Modelle, die der Anbieter anbietet, in die Liste holen"));
    modelRow->addWidget(m_chatModel, 1);
    modelRow->addWidget(load);
    f->addRow(T("Modell"), modelRow);
    auto* keyRow = new QHBoxLayout;
    m_chatKey = new QLineEdit;
    m_chatKey->setObjectName("ai_chat_key");
    m_chatKey->setEchoMode(QLineEdit::Password);
    m_keyRemove = new QPushButton(T("Entfernen"));
    m_keyRemove->setObjectName("ai_chat_key_remove");
    m_keyRemove->setToolTip(T("Den gespeicherten Schlüssel löschen"));
    keyRow->addWidget(m_chatKey, 1);
    keyRow->addWidget(m_keyRemove);
    f->addRow(T("Schlüssel"), keyRow);
    m_keyState = hint({});
    m_keyState->setObjectName("ai_chat_key_state");
    f->addRow(m_keyState);
    m_approval = new QComboBox;
    m_approval->setObjectName("ai_chat_approval");
    m_approval->addItem(T("Automatisch: jede Änderung gilt sofort"), int(AiSettings::Approval::Auto));
    m_approval->addItem(T("Annehmen: erst ansehen, dann übernehmen oder verwerfen"), int(AiSettings::Approval::Review));
    m_approval->setCurrentIndex(m_initial.approval == AiSettings::Approval::Review ? 1 : 0);
    f->addRow(T("Freigabe"), m_approval);
    f->addRow(hint(T("Automatisch: jede Änderung der KI ist ein eigener Rückgängig-Schritt. Annehmen: die Änderungen einer "
                     "Anfrage sind sofort in Timeline und Vorschau zu sehen; danach übernimmst du sie als einen Schritt "
                     "oder verwirfst sie.")));
    m_compact = new QComboBox;
    m_compact->setObjectName("ai_chat_compact");
    m_compact->addItem(T("Automatisch"), int(Ai::Compact::Auto));
    m_compact->addItem(T("Immer"), int(Ai::Compact::On));
    m_compact->addItem(T("Nie"), int(Ai::Compact::Off));
    m_compact->setCurrentIndex(m_compact->findData(int(m_initial.compact)));
    f->addRow(T("Kurze Anfrage für kleine Modelle"), m_compact);
    f->addRow(hint(T("Werkzeuge und Anleitung in Kurzform: etwa halb so viele Token, für Modelle mit wenig Kontext und "
                     "Modelle auf diesem Rechner (die lesen langsam). Die Einzelheiten holt sich die KI bei Bedarf. "
                     "Automatisch: für Server auf diesem Rechner und Modelle mit weniger als 32768 Token Kontext.")));
    auto* testRow = new QHBoxLayout;
    auto* test = new QPushButton(T("Verbindung testen"));
    test->setObjectName("ai_chat_test");
    test->setToolTip(T("Schickt eine winzige Anfrage an den Anbieter (kostet fast nichts)"));
    m_chatResult = new QLabel;
    m_chatResult->setObjectName("ai_chat_result");
    m_chatResult->setWordWrap(true);
    m_chatResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    testRow->addWidget(test);
    testRow->addWidget(m_chatResult, 1);
    f->addRow(testRow);
    if (m_company.found()) {
        const QString file = QDir::toNativeSeparators(m_company.file);
        QString text = choice.locked ? T("Anbieter und Adresse gibt deine Firma vor (%1).").arg(file)
                                     : T("Vorgaben deiner Firma aus %1.").arg(file);
        if (!m_company.error.isEmpty()) text += " " + T("Die Datei ist fehlerhaft: %1").arg(m_company.error);
        QLabel* note = hint(text);
        note->setObjectName("ai_chat_company");
        f->addRow(note);
    }
    if (choice.locked) {
        m_chatProvider->setEnabled(false);
        m_chatAddress->setEnabled(false);
    }

    connect(m_chatProvider, &QComboBox::currentIndexChanged, this, &AiSettingsPage::chatProviderChanged);
    connect(m_keyRemove, &QPushButton::clicked, this, [this] {
        QString error;
        if (!m_keys->remove(chatProvider(), &error)) m_chatResult->setText(T("Nicht gelöscht: %1").arg(error));
        updateKeyState();
    });
    connect(load, &QPushButton::clicked, this, [this, load] {
        load->setEnabled(false);
        m_chatResult->setText(T("Frage den Anbieter …"));
        m_client->listModels(chatEndpoint());
    });
    connect(m_client, &AiClient::models, this, [this, load](const QStringList& ids) {
        load->setEnabled(true);
        const QString current = m_chatModel->currentText();
        m_chatModel->clear();
        m_chatModel->addItems(ids);
        m_chatModel->setCurrentText(current);
        m_chatResult->setText(T("Modelle geladen: %1").arg(ids.size()));
    });
    connect(m_client, &AiClient::modelsFailed, this, [this, load](const QString& message) {
        load->setEnabled(true);
        m_chatResult->setText(message);
    });
    connect(test, &QPushButton::clicked, this, [this, test] {
        test->setEnabled(false);
        m_chatResult->setText(T("Teste …"));
        m_client->test(chatEndpoint());
    });
    connect(m_client, &AiClient::tested, this, [this, test](bool, const QString& message) {
        test->setEnabled(true);
        m_chatResult->setText(message);
    });
    chatProviderChanged();
    m_startProvider = chatProvider();
    m_startModel = m_chatModel->currentText().trimmed();
    if (choice.locked) m_chatAddress->setText(choice.address);
    else if (m_initial.chatProvider == chatProvider()) m_chatAddress->setText(m_initial.chatAddress);
}

QString AiSettingsPage::chatProvider() const
{
    return m_chatProvider ? m_chatProvider->currentData().toString() : m_initial.chatProvider;
}

void AiSettingsPage::chatProviderChanged()
{
    // The model typed for the provider shown so far stays (also when switching back)
    if (!m_shownProvider.isEmpty()) m_models.insert(m_shownProvider, m_chatModel->currentText().trimmed());
    const QString id = chatProvider();
    m_shownProvider = id;
    const Ai::Preset* p = Ai::preset(id);
    const Ai::Choice c = Ai::choose(id, {}, m_models, m_company);
    if (m_chatAddress->isEnabled()) m_chatAddress->clear();
    m_chatAddress->setPlaceholderText(c.address.isEmpty() ? QString("http://server:8000/v1") : c.address);
    m_chatModel->clear();
    if (p && !p->model.isEmpty()) m_chatModel->addItem(p->model);
    m_chatModel->setCurrentText(c.model);
    m_chatResult->clear();
    updateKeyState();
}

void AiSettingsPage::updateKeyState()
{
    const QString id = chatProvider();
    const Ai::Preset* p = Ai::preset(id);
    QString state;
    bool stored = false;
    for (const QString& var : Ai::keyVariables(id))
        if (state.isEmpty() && !qEnvironmentVariable(var.toLatin1().constData()).trimmed().isEmpty())
            state = T("Aus der Umgebungsvariable %1 (hat Vorrang).").arg(var);
    if (state.isEmpty() && m_company.found() && !m_company.key.isEmpty()
        && (m_company.provider.isEmpty() || m_company.provider == id))
        state = T("Aus der Firmen-Datei.");
    if (state.isEmpty() && m_keys->hasStored(id)) {
        stored = true;
        state = m_keys->secure() ? T("Gespeichert: %1.").arg(m_keys->storeName())
                                 : T("Unverschlüsselt in den Einstellungen gespeichert.");
    }
    if (state.isEmpty())
        state = p && !p->needsKey ? T("Für diesen Anbieter meist nicht nötig.") : T("Noch kein Schlüssel.");
    if (!m_keys->secure())
        state += " " + T("Kein Schlüsselbund des Systems gefunden: ein neuer Schlüssel wird unverschlüsselt in den "
                         "Einstellungen gespeichert.");
    m_keyState->setText(state);
    m_keyRemove->setVisible(stored);
    m_chatKey->setPlaceholderText(stored ? T("gespeichert – zum Ändern neu eingeben") : QString());
}

Ai::Endpoint AiSettingsPage::chatEndpoint()
{
    const QString id = chatProvider();
    const Ai::Preset* p = Ai::preset(id);
    QHash<QString, QString> models = m_models;
    models.insert(id, m_chatModel->currentText().trimmed());
    const Ai::Choice c = Ai::choose(id, m_chatAddress->text().trimmed(), models, m_company);
    Ai::Endpoint e;
    e.protocol = p ? p->protocol : Ai::Protocol::OpenAi;
    e.provider = id;
    e.address = c.address;
    e.model = c.model;
    e.key = m_chatKey->text().trimmed();
    if (e.key.isEmpty()) e.key = m_keys->resolve(id, m_company).key;
    return e;
}

bool AiSettingsPage::applyChatKey(QString* error)
{
    if (!m_chatKey || m_chatKey->text().trimmed().isEmpty()) return true;
    const bool ok = m_keys->store(chatProvider(), m_chatKey->text().trimmed(), error);
    if (ok) m_chatKey->clear();
    return ok;
}
