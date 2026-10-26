#include "app/AiSettingsPage.h"

#include "app/Theme.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
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
    connect(m_highlight, &QCheckBox::toggled, this, &AiSettingsPage::updateEnabled);

    // The indicator next to the project name
    f = formOf(addSection(T("Anzeige oben")));
    m_indicator = new QComboBox;
    m_indicator->addItem(T("Solange eine KI verbunden ist"), int(AiSettings::Indicator::WhileActive));
    m_indicator->addItem(T("Immer"), int(AiSettings::Indicator::Always));
    m_indicator->addItem(T("Nie"), int(AiSettings::Indicator::Never));
    m_indicator->setCurrentIndex(std::max(0, m_indicator->findData(int(s.indicator))));
    f->addRow(T("KI-Anzeige"), m_indicator);

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
    for (QWidget* w : {static_cast<QWidget*>(m_colorButton), static_cast<QWidget*>(m_duration)}) w->setEnabled(on);
    m_colorReset->setEnabled(on && m_color != AiSettings::defaultColor());
}

AiSettings AiSettingsPage::settings() const
{
    AiSettings s = m_initial;
    s.liveAllowed = m_liveAllowed->isChecked();
    s.highlight = m_highlight->isChecked();
    s.color = m_color;
    s.highlightMs = int(std::lround(m_duration->value() * 1000));
    s.indicator = AiSettings::Indicator(m_indicator->currentData().toInt());
    return s;
}
