#include "app/SettingsDialog.h"

#include "app/AiSettingsPage.h"
#include "app/Theme.h"
#include "core/I18n.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

// Pages scroll when they get longer than the dialog (the AI page grows with the chat settings)
QScrollArea* scrolling(QWidget* page)
{
    auto* area = new QScrollArea;
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

} // namespace

SettingsDialog::SettingsDialog(const AiSettings& ai, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(T("Einstellungen"));
    m_tabs = new QTabWidget;
    m_tabs->addTab(scrolling(buildGeneral()), T("Allgemein"));
    m_aiPage = new AiSettingsPage(ai);
    m_tabs->addTab(scrolling(m_aiPage), T("KI"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(m_tabs, 1);
    lay->addWidget(buttons);
    resize(560, 560);
}

QWidget* SettingsDialog::buildGeneral()
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    // Look: language (takes effect after a restart, like from the menu), design and keys in their own dialogs
    auto* look = new QGroupBox(T("Oberfläche"));
    auto* form = new QFormLayout(look);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_language = new QComboBox;
    for (const I18n::Language& l : I18n::languages()) m_language->addItem(l.name, l.code);
    m_language->setCurrentIndex(std::max(0, m_language->findData(I18n::language())));
    form->addRow(T("Sprache"), m_language);
    auto* buttons = new QHBoxLayout;
    auto* design = new QPushButton(T("Design…"));
    connect(design, &QPushButton::clicked, this, &SettingsDialog::designRequested);
    auto* keys = new QPushButton(T("Tastenbelegung…"));
    connect(keys, &QPushButton::clicked, this, &SettingsDialog::keyBindingsRequested);
    buttons->addWidget(design);
    buttons->addWidget(keys);
    buttons->addStretch(1);
    form->addRow(buttons);
    form->addRow(hint(T("Sprache und Design ändern sich nach dem Neustart von schneidi.")));
    lay->addWidget(look);
    lay->addStretch(1);
    return page;
}

void SettingsDialog::showTab(Tab tab) { m_tabs->setCurrentIndex(tab == Tab::Ai ? 1 : 0); }

AiSettings SettingsDialog::aiSettings() const { return m_aiPage->settings(); }

QString SettingsDialog::language() const { return m_language->currentData().toString(); }
