// SPDX-License-Identifier: AGPL-3.0-only
#include "app/SettingsDialog.h"
#include "app/UiScale.h"
#include "ui/touch/Touch.h"

#include "app/AiSettingsPage.h"
#include "app/StartTemplatesPage.h"
#include "app/TimelineSettingsPage.h"
#include "app/Theme.h"
#include "app/UpdateCheck.h"
#include "core/AppPaths.h"
#include "core/SettingsIsolation.h"
#include "core/I18n.h"
#include "engine/Engine.h"
#include "engine/audio/Device.h"
#include "engine/Background.h"
#include "ui/comp/CompShared.h"
#include "ui/WheelGuard.h"

#include <QCheckBox>
#include <QComboBox>
#include <QStandardItemModel>
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
#include <algorithm>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

namespace {

// Scrolls only downwards: the dialog is at least as wide as the page's narrowest layout (long labels already put
// their fields below them, hints wrap), so nothing is cut off at the right in a long language with a large font
class PageArea : public QScrollArea {
public:
    bool narrow = false; // a phone: as narrow as the screen, long rows wrap (SettingsDialog::fitScreen)
    QSize minimumSizeHint() const override
    {
        QSize s = QScrollArea::minimumSizeHint();
        if (widget() && !narrow)
            s.setWidth(std::max(s.width(), widget()->minimumSizeHint().width() + verticalScrollBar()->sizeHint().width()
                                               + 2 * frameWidth()));
        return s;
    }
};

// Pages scroll when they get longer than the dialog (the AI page grows with the chat settings)
QScrollArea* scrolling(QWidget* page)
{
    auto* area = new PageArea;
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
    m_timelinePage = new TimelineSettingsPage;
    m_timelineTab = scrolling(m_timelinePage);
    m_tabs->addTab(m_timelineTab, T("Timeline"));
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

void SettingsDialog::fitScreen(const QRect& room)
{
    if (!room.isValid() || (room.width() >= width() && room.height() >= height())) return;
    // A phone: the dialog fills the screen (its OK/Cancel at the bottom in sight), the pages scroll inside it and get
    // as narrow as the screen - a long label puts its field below it, what is still wider scrolls sideways
    for (QWidget* area : {m_generalTab, m_templatesTab, m_aiTab})
        if (auto* page = dynamic_cast<PageArea*>(area)) {
            // (the page's room: the screen less the dialog's, the page's and its group boxes' margins)
            Touch::narrowForPhone(page->widget(), room.width() - 72);
            page->narrow = true;
            page->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
            page->updateGeometry();
        }
    m_tabs->setUsesScrollButtons(true);
    m_tabs->tabBar()->setExpanding(false);
    setMinimumSize(0, 0);
    setMaximumSize(room.size());
    setGeometry(room);
    m_fitRoom = room;
}

void SettingsDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    if (m_fitRoom.isValid()) setGeometry(m_fitRoom); // (a platform may place a new window by its own idea)
}

QWidget* SettingsDialog::buildGeneral()
{
    auto* page = new QWidget;
    auto* lay = new QVBoxLayout(page);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(10);

    // Look: language (applied at once with OK, like from the menu), design and keys in their own dialogs
    auto* look = new QGroupBox(T("Oberfläche"));
    auto* form = new QFormLayout(look);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_language = new QComboBox;
    for (const I18n::Language& l : I18n::languages()) m_language->addItem(l.name, l.code);
    m_language->setCurrentIndex(std::max(0, m_language->findData(I18n::language())));
    form->addRow(T("Sprache"), m_language);
    // The whole interface larger (fonts, panels, icons): for a 4K screen at 100 % (app/UiScale.h)
    m_uiScale = new QComboBox;
    m_uiScale->addItem(T("Wie das System"), 0.0);
    const auto percent = [](double f) { return QString::number(qRound(f * 100)) + " %"; };
    for (double f : UiScale::steps()) m_uiScale->addItem(percent(f), f);
    // A saved size the list lacks (set by hand in the settings file): shown as it is, OK keeps it
    if (const double saved = UiScale::saved(); saved > 0 && m_uiScale->findData(saved) < 0)
        m_uiScale->addItem(percent(saved), saved);
    m_uiScale->setCurrentIndex(std::max(0, m_uiScale->findData(UiScale::saved())));
    form->addRow(T("Größe"), m_uiScale);
    // Larger controls for fingers (ui/touch/Touch.h); auto = on for a touchscreen without mouse or touchpad
    m_touchMode = new QComboBox;
    m_touchMode->setObjectName("settings_touch_mode");
    m_touchMode->addItem(T("Automatisch"), int(Touch::Mode::Auto));
    m_touchMode->addItem(T("Immer"), int(Touch::Mode::On));
    m_touchMode->addItem(T("Nie"), int(Touch::Mode::Off));
    m_touchMode->setCurrentIndex(std::max(0, m_touchMode->findData(int(Touch::savedMode()))));
    m_touchMode->setToolTip(T("Größere Knöpfe, Zeilen, Spuren und Griffe für die Bedienung mit dem Finger. "
                              "Automatisch: an bei einem Touchscreen ohne Maus und Touchpad."));
    form->addRow(T("Touch-Modus"), m_touchMode);
    // Viewer, timeline and tools stacked, one panel at a time (MainWindow::phoneLayoutWanted)
    m_phoneLayout = new QComboBox;
    m_phoneLayout->setObjectName("settings_phone_layout");
    m_phoneLayout->addItem(T("Automatisch"), QString());
    m_phoneLayout->addItem(T("Handy"), QStringLiteral("phone"));
    m_phoneLayout->addItem(T("Tablet"), QStringLiteral("tablet"));
    m_phoneLayout->addItem(T("Desktop"), QStringLiteral("desktop"));
    m_phoneLayout->setCurrentIndex(std::max(0, m_phoneLayout->findData(savedPhoneLayout())));
    m_phoneLayout->setToolTip(T("Handy: Vorschau, Timeline und Werkzeuge untereinander, ein Bereich nach dem anderen. "
                                "Tablet: Werkzeugleiste links, ein Bereich fest daneben. Automatisch: nach der "
                                "Fenstergröße, wechselt sofort."));
    form->addRow(T("Anordnung"), m_phoneLayout);
    m_reduceMotion = new QCheckBox(T("Bewegungen reduzieren"));
    m_reduceMotion->setObjectName("settings_reduce_motion");
    m_reduceMotion->setChecked(QSettings().value("ui/reduceMotion", false).toBool());
    m_reduceMotion->setToolTip(T("Wechselt die Anordnung (Fenstergröße, Klapphandy), ohne Überblenden."));
    form->addRow(QString(), m_reduceMotion);
    // The page shown at the start and on New Project (MainWindow::showStartPage); filled by setStartPages
    m_startPage = new QComboBox;
    m_startPage->setObjectName("settings_start_page");
    m_startPage->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_startPage->setMinimumContentsLength(8);
    form->addRow(T("Startseite"), m_startPage);
    if (const QString own = UiScale::userFactor(); !own.isEmpty())
        form->addRow(hint(T("Wird von QT_SCALE_FACTOR=%1 überschrieben (beim Start gesetzt).").arg(own)));
    auto* buttons = new QHBoxLayout;
    auto* design = new QPushButton(T("Design…"));
    connect(design, &QPushButton::clicked, this, &SettingsDialog::designRequested);
    auto* keys = new QPushButton(T("Tastenbelegung…"));
    connect(keys, &QPushButton::clicked, this, &SettingsDialog::keyBindingsRequested);
    buttons->addWidget(design);
    buttons->addWidget(keys);
    buttons->addStretch(1);
    form->addRow(buttons);
    form->addRow(hint(T("Sprache und Design wechseln sofort. Eine neue Größe braucht einen Neustart; das Projekt und die "
                        "Ansicht kommen danach so zurück, wie sie waren.")));
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
    m_proxyQuality = new QComboBox;
    m_proxyQuality->setObjectName("proxy_quality");
    using Q = ProxyManager::Quality;
    m_proxyQuality->addItem(T("Klein (540p, am schnellsten)"), int(Q::Small));
    m_proxyQuality->addItem(T("Mittel (720p)"), int(Q::Medium));
    m_proxyQuality->addItem(T("Groß (1080p)"), int(Q::Large));
    m_proxyQuality->addItem(T("Volle Auflösung (nur leichter Codec)"), int(Q::Full));
    m_proxyQuality->setCurrentIndex(std::max(0, m_proxyQuality->findData(int(ProxyManager::quality()))));
    xform->addRow(T("Qualität"), m_proxyQuality);
    xform->addRow(hint(T("Proxy-Medien sind kleine Kopien für eine flüssige Vorschau, der Export nutzt immer die "
                         "Originale. Von Hand: Wiedergabe › Proxy-Medien erzeugen oder Rechtsklick auf einen Clip.")));
    lay->addWidget(proxyBox);

    // Timeline: the playhead fixed in the middle (phones' editors) - automatic = on the phone and tablet forms
    auto* timelineBox = new QGroupBox(T("Timeline"));
    auto* tform = new QFormLayout(timelineBox);
    m_centerPlayhead = new QComboBox;
    m_centerPlayhead->setObjectName("settings_center_playhead");
    m_centerPlayhead->addItem(T("Automatisch (Handy, Tablet)"), QString());
    m_centerPlayhead->addItem(T("Immer"), QStringLiteral("on"));
    m_centerPlayhead->addItem(T("Nie"), QStringLiteral("off"));
    m_centerPlayhead->setCurrentIndex(std::max(0, m_centerPlayhead->findData(QSettings().value("timeline/centerPlayhead").toString())));
    tform->addRow(T("Abspielmarke in der Mitte"), m_centerPlayhead);
    tform->addRow(hint(T("Die Abspielmarke bleibt in der Mitte stehen, die Timeline gleitet darunter: zur Seite ziehen "
                         "spult, beim Abspielen läuft die Timeline mit.")));
    // Phone and tablet: how a finger handles clips (as phone editors do by default)
    m_magneticMain = new QCheckBox(T("Hauptspur magnetisch"));
    m_magneticMain->setObjectName("settings_magnetic_main");
    m_magneticMain->setChecked(QSettings().value("timeline/magneticMain", true).toBool());
    m_magneticMain->setToolTip(T("Auf Handy und Tablet: Clips auf V1 liegen ohne Lücken hintereinander; gedrückt halten "
                                 "und ziehen sortiert sie um. Aus: Lücken erlaubt wie am Desktop."));
    tform->addRow(QString(), m_magneticMain);
    m_clipDrag = new QComboBox;
    m_clipDrag->setObjectName("settings_clip_drag");
    m_clipDrag->addItem(T("Gedrückt halten"), QStringLiteral("hold"));
    m_clipDrag->addItem(T("Sofort"), QStringLiteral("instant"));
    m_clipDrag->setCurrentIndex(std::max(0, m_clipDrag->findData(QSettings().value("timeline/clipDrag", "hold").toString())));
    m_clipDrag->setToolTip(T("Auf Handy und Tablet. Gedrückt halten: Wischen spult auch über Clips, ein Clip bewegt sich "
                             "erst nach kurzem Halten. Sofort: ein Finger auf einem Clip verschiebt ihn gleich."));
    tform->addRow(T("Clip ziehen"), m_clipDrag);
    // How far the timeline glides after a flick of the finger (phone and tablet)
    m_momentum = new QComboBox;
    m_momentum->setObjectName("settings_timeline_momentum");
    m_momentum->addItem(T("Wenig"), QStringLiteral("low"));
    m_momentum->addItem(T("Normal"), QStringLiteral("normal"));
    m_momentum->addItem(T("Aus"), QStringLiteral("off"));
    m_momentum->setCurrentIndex(std::max(0, m_momentum->findData(QSettings().value("timeline/momentum", "low").toString())));
    m_momentum->setToolTip(T("Wie weit die Timeline nach einem schnellen Wischen weitergleitet. Langsames Ziehen hält an "
                             "Clip-Anfängen und -Enden kurz an."));
    tform->addRow(T("Schwung der Timeline"), m_momentum);
    lay->addWidget(timelineBox);

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
        for (const PreviewResolution::Choice c : PreviewResolution::kChoices)
            box->addItem(PreviewResolution::label(c), int(c));
        box->setToolTip(T("Feste Größen nennen die kürzere Seite, die längere folgt dem Seitenverhältnis des "
                          "Projekts (ein 9:16-Projekt bei 1080p: 1080×1920)."));
        box->setCurrentIndex(std::max(0, box->findData(int(PreviewResolution::stored(target)))));
        return box;
    };
    auto* viewerBox = new QGroupBox(T("Vorschau"));
    auto* rform = new QFormLayout(viewerBox);
    rform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_viewerResolution = resolutionBox("viewer_resolution", PreviewResolution::Target::Viewer);
    rform->addRow(T("Auflösung"), m_viewerResolution);
    rform->addRow(hint(T("Automatisch: so groß, wie die Vorschau gerade angezeigt wird. Mehr Pixel sind schärfer, "
                         "brauchen aber einen schnellen Rechner. Der Export nutzt immer die volle Auflösung.")));
    lay->addWidget(viewerBox);

    // Sound output (also in Playback › Audio › Output device); the list is read now: devices come and go
    auto* soundBox = new QGroupBox(T("Ton"));
    auto* sform = new QFormLayout(soundBox);
    sform->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_audioOutput = new QComboBox;
    m_audioOutput->setObjectName("audio_output");
    m_audioOutput->addItem(T("Systemstandard"), QString());
    const QString currentOutput = Engine::audioOutput();
    bool found = currentOutput.isEmpty();
    for (const Audio::Device::Output& o : Audio::Device::outputs(Engine::audioDriver())) {
        m_audioOutput->addItem(o.name, o.id);
        found = found || o.id == currentOutput;
    }
    if (!found) m_audioOutput->addItem(T("%1 (nicht angeschlossen)").arg(currentOutput), currentOutput);
    m_audioOutput->setCurrentIndex(std::max(0, m_audioOutput->findData(currentOutput)));
    sform->addRow(T("Ausgabegerät"), m_audioOutput);
    sform->addRow(hint(T("Wohin schneidi den Ton spielt. Systemstandard folgt dem Gerät, das im System gewählt ist.")));
    lay->addWidget(soundBox);

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

ZoomAnchor::Mode SettingsDialog::zoomAnchor() const { return m_timelinePage->zoomAnchor(); }

PlayheadMove::Mode SettingsDialog::afterPlayheadMove() const { return m_timelinePage->afterPlayheadMove(); }

SecondScreenPlan::Mode SettingsDialog::secondScreenMode() const
{
    return SecondScreenPlan::Mode(m_secondScreen->currentData().toInt());
}

void SettingsDialog::setPreviewProject(const QSize& project)
{
    for (QComboBox* box : {m_viewerResolution, m_monitorResolution}) {
        auto* model = qobject_cast<QStandardItemModel*>(box->model());
        for (int i = 0; i < box->count(); ++i) {
            const auto c = PreviewResolution::Choice(box->itemData(i).toInt());
            const bool larger = PreviewResolution::largerThanProject(c, project);
            box->setItemText(i, larger ? T("%1 (größer als das Projekt)").arg(PreviewResolution::label(c))
                                       : PreviewResolution::label(c));
            if (QStandardItem* item = model ? model->item(i) : nullptr)
                item->setEnabled(!larger || i == box->currentIndex());
        }
    }
}

PreviewResolution::Choice SettingsDialog::previewResolution(PreviewResolution::Target target) const
{
    const QComboBox* box = target == PreviewResolution::Target::Viewer ? m_viewerResolution : m_monitorResolution;
    return PreviewResolution::Choice(box->currentData().toInt());
}

QString SettingsDialog::audioOutput() const
{
    return m_audioOutput->currentData().toString();
}

ProxyManager::Quality SettingsDialog::proxyQuality() const
{
    return ProxyManager::Quality(m_proxyQuality->currentData().toInt());
}

ProxyManager::ImportRule SettingsDialog::proxyImportRule() const
{
    return ProxyManager::ImportRule(m_proxyImport->currentData().toInt());
}

bool SettingsDialog::ownProjectFolder() const { return m_ownFolder->isChecked(); }

QString SettingsDialog::projectFolder() const { return QDir::fromNativeSeparators(m_folder->text().trimmed()); }

bool SettingsDialog::storedOwnFolder() { return QSettings().value("project/ownFolder", false).toBool(); }

QString SettingsDialog::storedFolder() { return AppPaths::projectFolder(); }

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
    m_tabs->setCurrentWidget(tab == Tab::Ai          ? m_aiTab
                             : tab == Tab::Templates ? m_templatesTab
                             : tab == Tab::Timeline  ? m_timelineTab
                                                     : m_generalTab);
}

AiSettings SettingsDialog::aiSettings() const { return m_aiPage->settings(); }

void SettingsDialog::setStartPages(const QVector<QPair<QString, QString>>& pages, const QString& current)
{
    m_startPage->clear();
    for (const auto& [id, name] : pages) m_startPage->addItem(name, id);
    m_startPage->setCurrentIndex(std::max(0, m_startPage->findData(current)));
}

QString SettingsDialog::startPage() const { return m_startPage->currentData().toString(); }

QString SettingsDialog::language() const { return m_language->currentData().toString(); }

double SettingsDialog::uiScale() const { return m_uiScale->currentData().toDouble(); }
bool SettingsDialog::magneticMain() const { return m_magneticMain->isChecked(); }
QString SettingsDialog::clipDrag() const { return m_clipDrag->currentData().toString(); }
QString SettingsDialog::timelineMomentum() const { return m_momentum->currentData().toString(); }

QString SettingsDialog::centerPlayhead() const { return m_centerPlayhead->currentData().toString(); }

bool SettingsDialog::reduceMotion() const { return m_reduceMotion->isChecked(); }

QString SettingsDialog::phoneLayout() const { return m_phoneLayout->currentData().toString(); }

QString SettingsDialog::savedPhoneLayout()
{
    QSettings s;
    const QString v = s.value("ui/layoutForm").toString();
    if (v == "phone" || v == "tablet" || v == "desktop") return v;
    const QString old = s.value("ui/phoneLayout").toString(); // (the 0.26 prototype's on/off)
    return old == "on" ? QStringLiteral("phone") : old == "off" ? QStringLiteral("desktop") : QString();
}

void SettingsDialog::setSavedPhoneLayout(const QString& value)
{
    QSettings s;
    s.remove("ui/phoneLayout");
    if (value.isEmpty()) s.remove("ui/layoutForm");
    else s.setValue("ui/layoutForm", value);
}

Touch::Mode SettingsDialog::touchMode() const { return Touch::Mode(m_touchMode->currentData().toInt()); }
