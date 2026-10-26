#include "app/SettingsDialog.h"

#include "app/AiSettingsPage.h"
#include "app/Theme.h"
#include "app/UpdateCheck.h"
#include "core/I18n.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
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

    // Update hint (the check itself happens in the main window; "Check now" asks right here)
    auto* updates = new QGroupBox(T("Updates"));
    auto* uform = new QFormLayout(updates);
    m_updateHint = new QCheckBox(T("Beim Start nach neuen Versionen suchen (höchstens einmal am Tag)"));
    m_updateHint->setChecked(UpdateCheck::enabled());
    uform->addRow(m_updateHint);
    auto* checkRow = new QHBoxLayout;
    m_checkNow = new QPushButton(T("Jetzt suchen"));
    m_checkNow->setObjectName("update_check_now");
    m_updateResult = new QLabel;
    m_updateResult->setObjectName("update_result");
    m_updateResult->setWordWrap(true);
    m_updateResult->setOpenExternalLinks(true);
    m_updateResult->setTextInteractionFlags(Qt::TextBrowserInteraction);
    checkRow->addWidget(m_checkNow);
    checkRow->addWidget(m_updateResult, 1);
    uform->addRow(checkRow);
    uform->addRow(hint(T("Fragt GitHub nach der neuesten Version; mit geht nur die Versionsnummer, nichts über deine "
                         "Projekte. Neue Versionen gibt es auf der Release-Seite.")));
    lay->addWidget(updates);
    m_updates = new UpdateCheck(this);
    const QString current = UpdateCheck::runningVersion(QCoreApplication::applicationVersion());
    connect(m_checkNow, &QPushButton::clicked, this, [this, current] {
        m_checkNow->setEnabled(false);
        m_updateResult->setText(T("Suche…"));
        m_updates->check(current);
    });
    connect(m_updates, &UpdateCheck::newer, this, [this](const UpdateCheck::Release& r, const QString& cur) {
        m_checkNow->setEnabled(true);
        m_updateResult->setText(T("schneidi %1 ist da (du hast %2).").arg(r.version, cur) + QString(" <a href=\"%1\">%2</a>")
                                    .arg(r.page.toString().toHtmlEscaped(), T("Release-Seite öffnen")));
    });
    connect(m_updates, &UpdateCheck::upToDate, this, [this](const QString& cur) {
        m_checkNow->setEnabled(true);
        m_updateResult->setText(T("schneidi ist aktuell (%1).").arg(cur).toHtmlEscaped());
    });
    connect(m_updates, &UpdateCheck::failed, this, [this](const QString& message) {
        m_checkNow->setEnabled(true);
        m_updateResult->setText(T("Suche fehlgeschlagen: %1").arg(message).toHtmlEscaped());
    });
    lay->addStretch(1);
    return page;
}

bool SettingsDialog::updateHint() const { return m_updateHint->isChecked(); }

void SettingsDialog::showTab(Tab tab) { m_tabs->setCurrentIndex(tab == Tab::Ai ? 1 : 0); }

AiSettings SettingsDialog::aiSettings() const { return m_aiPage->settings(); }

QString SettingsDialog::language() const { return m_language->currentData().toString(); }
