#include "app/AiSettingsPage.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
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
    s.highlight = m_highlight->isChecked();
    s.color = m_color;
    s.highlightMs = int(std::lround(m_duration->value() * 1000));
    s.distinguish = m_distinguish->isChecked();
    s.indicator = AiSettings::Indicator(m_indicator->currentData().toInt());
    s.logLength = m_logLength->value();
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
