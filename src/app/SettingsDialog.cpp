// SPDX-License-Identifier: AGPL-3.0-only
#include "app/SettingsDialog.h"

#include "app/AiSettingsPage.h"
#include "app/StartTemplatesPage.h"
#include "app/Theme.h"
#include "app/UpdateCheck.h"
#include "core/I18n.h"
#include "engine/Background.h"
#include "ui/comp/CompShared.h"
#include "ui/WheelGuard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

namespace {

// Pages scroll when they get longer than the dialog (the AI page grows with the chat settings)
QScrollArea* scrolling(QWidget* page)
{
    auto* area = new QScrollArea;
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // long hints wrap instead
    WheelGuard::install(page); // scrolling the page over a box must not change it
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
    m_generalTab = scrolling(buildGeneral());
    m_tabs->addTab(m_generalTab, T("Allgemein"));
    m_templatesPage = new StartTemplatesPage;
    m_templatesTab = scrolling(m_templatesPage);
    m_tabs->addTab(m_templatesTab, T("Startvorlagen"));
    m_aiPage = new AiSettingsPage(ai);
    m_aiTab = scrolling(m_aiPage);
    m_tabs->addTab(m_aiTab, T("KI"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(m_tabs, 1);
    lay->addWidget(buttons);
    // Room for the longest page without scrolling (Start Templates with its buttons below the list); fits a 768 screen
    resize(640, 680);
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
                         "Projekte. Neue Versionen gibt es auf der Download-Seite.")));
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
                                    .arg(r.page.toString().toHtmlEscaped(), T("Download-Seite öffnen")));
    });
    connect(m_updates, &UpdateCheck::upToDate, this, [this](const QString& cur) {
        m_checkNow->setEnabled(true);
        m_updateResult->setText(T("schneidi ist aktuell (%1).").arg(cur).toHtmlEscaped());
    });
    connect(m_updates, &UpdateCheck::failed, this, [this](const QString& message) {
        m_checkNow->setEnabled(true);
        m_updateResult->setText(T("Suche fehlgeschlagen: %1").arg(message).toHtmlEscaped());
    });

    // Threads for background work (whisper above all keeps every thread it gets busy for minutes)
    auto* performance = new QGroupBox(T("Leistung"));
    auto* pform = new QFormLayout(performance);
    pform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_threads = new QComboBox;
    m_threads->setObjectName("background_threads");
    m_threads->addItem(T("Automatisch"), 0);
    for (int n = std::max(1, QThread::idealThreadCount()); n >= 1; --n) m_threads->addItem(QString::number(n), n);
    m_threads->setCurrentIndex(std::max(0, m_threads->findData(Background::chosenThreads())));
    pform->addRow(T("Threads für Hintergrundarbeit"), m_threads);
    pform->addRow(hint(T("Transkription (Whisper), Analysen, Proxys und das Einlesen beim Import. Weniger Threads: "
                         "Musik, Spiele und andere Programme laufen nebenbei flüssiger, die Arbeit dauert länger. "
                         "Den Export stellst du auf der Seite Export ein.")));
    lay->addWidget(performance);

    // Projects: "Save as" next to the footage (default) or in a folder of their own, only if the user wants that
    auto* projects = new QGroupBox(T("Projekte"));
    auto* prform = new QFormLayout(projects);
    prform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_ownFolder = new QCheckBox(T("Neue Projekte in einem eigenen Ordner speichern"));
    m_ownFolder->setObjectName("project_own_folder");
    m_ownFolder->setChecked(storedOwnFolder());
    prform->addRow(m_ownFolder);
    auto* folderRow = new QHBoxLayout;
    m_folder = new QLineEdit(storedFolder());
    m_folder->setObjectName("project_folder");
    auto* browse = new QPushButton(T("Wählen…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, T("Ordner für Projekte"), m_folder->text());
        if (!dir.isEmpty()) m_folder->setText(QDir::toNativeSeparators(dir));
    });
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    prform->addRow(T("Ordner"), folderRow);
    const auto enable = [this, browse](bool on) {
        m_folder->setEnabled(on);
        browse->setEnabled(on);
    };
    enable(m_ownFolder->isChecked());
    connect(m_ownFolder, &QCheckBox::toggled, this, enable);
    prform->addRow(hint(T("Aus: „Speichern unter“ schlägt den Ordner des ersten Clips vor. An: dieser Ordner, so landen "
                          "keine Projektdateien zwischen deinen Aufnahmen.")));
    lay->addWidget(projects);

    // Proxy media on import: never by default (the user decides; Playback › Generate Proxy Media, right-click on clips)
    auto* proxyBox = new QGroupBox(T("Proxy-Medien"));
    auto* xform = new QFormLayout(proxyBox);
    xform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_proxyImport = new QComboBox;
    m_proxyImport->setObjectName("proxy_import_rule");
    m_proxyImport->addItem(T("Nie"), int(ProxyManager::ImportRule::Never));
    m_proxyImport->addItem(T("Ab 4K, H.265, AV1 oder 10 Bit"), int(ProxyManager::ImportRule::Heavy));
    m_proxyImport->addItem(T("Immer"), int(ProxyManager::ImportRule::Always));
    m_proxyImport->setCurrentIndex(std::max(0, m_proxyImport->findData(int(ProxyManager::importRule()))));
    xform->addRow(T("Beim Import erzeugen"), m_proxyImport);
    xform->addRow(hint(T("Proxy-Medien sind kleine Kopien für eine flüssige Vorschau, der Export nutzt immer die "
                         "Originale. Von Hand: Wiedergabe › Proxy-Medien erzeugen oder Rechtsklick auf einen Clip.")));
    lay->addWidget(proxyBox);

    // Compositing: new nodes from the node search linked at once (default) or only added
    auto* comp = new QGroupBox(T("Compositing"));
    auto* cform = new QFormLayout(comp);
    m_autoLink = new QCheckBox(T("Neue Knoten automatisch verbinden"));
    m_autoLink->setObjectName("comp_auto_link");
    m_autoLink->setChecked(CompUi::autoLink());
    cform->addRow(m_autoLink);
    cform->addRow(hint(T("An: ein neuer Knoten kommt hinter den gewählten (oder den Ausgang). Aus: er wird nur "
                         "hingelegt, du verbindest ihn selbst.")));
    lay->addWidget(comp);

    // Preview resolution (engine/PreviewResolution): automatic by default, playback speed first
    const auto resolutionBox = [](const char* name, PreviewResolution::Target target) {
        auto* box = new QComboBox;
        box->setObjectName(name);
        box->addItem(T("Automatisch (passend zur Anzeige)"), int(PreviewResolution::Choice::Auto));
        box->addItem(T("Halbe Auflösung"), int(PreviewResolution::Choice::Half));
        box->addItem(T("Volle Projektauflösung"), int(PreviewResolution::Choice::Full));
        box->setCurrentIndex(std::max(0, box->findData(int(PreviewResolution::stored(target)))));
        return box;
    };
    auto* viewerBox = new QGroupBox(T("Vorschau"));
    auto* rform = new QFormLayout(viewerBox);
    rform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_viewerResolution = resolutionBox("viewer_resolution", PreviewResolution::Target::Viewer);
    rform->addRow(T("Auflösung"), m_viewerResolution);
    rform->addRow(hint(T("Automatisch: so groß, wie die Vorschau gerade angezeigt wird. Volle Auflösung ist schärfer, "
                         "braucht aber einen schnellen Rechner. Der Export nutzt immer die volle Auflösung.")));
    lay->addWidget(viewerBox);

    // Preview on a second monitor (Workspace menu): what the main window does with the viewer's area meanwhile
    auto* preview = new QGroupBox(T("Vorschau auf zweitem Monitor"));
    auto* vform = new QFormLayout(preview);
    vform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_monitorResolution = resolutionBox("monitor_resolution", PreviewResolution::Target::Monitor);
    vform->addRow(T("Auflösung auf dem Monitor"), m_monitorResolution);
    m_secondScreen = new QComboBox;
    m_secondScreen->setObjectName("second_screen_mode");
    m_secondScreen->addItem(T("Quellclips zeigen"), int(SecondScreenPlan::Mode::KeepArea));
    m_secondScreen->addItem(T("Vorschau-Fläche ausblenden"), int(SecondScreenPlan::Mode::HideViewer));
    m_secondScreen->setCurrentIndex(std::max(0, m_secondScreen->findData(int(SecondScreenPlan::storedMode()))));
    vform->addRow(T("Im Hauptfenster"), m_secondScreen);
    vform->addRow(hint(T("Die Timeline läuft dann nur auf dem Monitor. Quellclips zeigen: die Vorschau-Fläche im "
                         "Hauptfenster zeigt Clips, die du per Doppelklick öffnest. Ausblenden: die Bereiche daneben "
                         "bekommen den Platz.")));
    lay->addWidget(preview);

    lay->addStretch(1);
    return page;
}

bool SettingsDialog::autoLinkNodes() const { return m_autoLink->isChecked(); }

SecondScreenPlan::Mode SettingsDialog::secondScreenMode() const
{
    return SecondScreenPlan::Mode(m_secondScreen->currentData().toInt());
}

PreviewResolution::Choice SettingsDialog::previewResolution(PreviewResolution::Target target) const
{
    const QComboBox* box = target == PreviewResolution::Target::Viewer ? m_viewerResolution : m_monitorResolution;
    return PreviewResolution::Choice(box->currentData().toInt());
}

ProxyManager::ImportRule SettingsDialog::proxyImportRule() const
{
    return ProxyManager::ImportRule(m_proxyImport->currentData().toInt());
}

bool SettingsDialog::ownProjectFolder() const { return m_ownFolder->isChecked(); }

QString SettingsDialog::projectFolder() const { return QDir::fromNativeSeparators(m_folder->text().trimmed()); }

bool SettingsDialog::storedOwnFolder() { return QSettings().value("project/ownFolder", false).toBool(); }

QString SettingsDialog::storedFolder()
{
    const QString stored = QSettings().value("project/folder").toString();
    if (!stored.isEmpty()) return stored;
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(movies.isEmpty() ? QDir::homePath() : movies).filePath("schneidi");
}

void SettingsDialog::store(bool own, const QString& folder)
{
    QSettings s;
    s.setValue("project/ownFolder", own);
    s.setValue("project/folder", folder);
}

bool SettingsDialog::updateHint() const { return m_updateHint->isChecked(); }


int SettingsDialog::backgroundThreads() const { return m_threads->currentData().toInt(); }

void SettingsDialog::showTab(Tab tab)
{
    m_tabs->setCurrentWidget(tab == Tab::Ai ? m_aiTab : tab == Tab::Templates ? m_templatesTab : m_generalTab);
}

AiSettings SettingsDialog::aiSettings() const { return m_aiPage->settings(); }

QString SettingsDialog::language() const { return m_language->currentData().toString(); }
