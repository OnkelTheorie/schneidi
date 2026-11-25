// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "app/AiSettings.h"
#include "engine/PreviewResolution.h"
#include "engine/ProxyManager.h"
#include "ui/SecondScreen.h"
#include "ui/touch/Touch.h"
#include "ui/timeline/PlayheadMove.h"
#include "ui/timeline/ZoomAnchor.h"

#include <QDialog>

class AiSettingsPage;
class QCheckBox;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class StartTemplatesPage;
class TimelineSettingsPage;
class UpdateCheck;

// Settings of the app (File › Settings…, Ctrl+,), the same for all projects; project settings stay in their own
// dialog. Tabs: General (language, design, keys, update hint, threads for background work), Timeline
// (TimelineSettingsPage: how the timelines behave where editors differ), Start Templates (StartTemplatesPage: how new projects look; its file actions take effect at once) and AI
// (AiSettingsPage). Nothing else is applied before OK ("Check now" for updates asks right away); the caller reads the
// choices and applies/saves them (MainWindow::settingsDialog). Options that already have a menu entry keep it (the menus stay where users of other
// editors look for them).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    enum class Tab { General, Ai, Templates, Timeline };

    SettingsDialog(const AiSettings& ai, QWidget* parent = nullptr);
    void showTab(Tab tab);
    // Smaller than the dialog's own size (a phone): fills `room` (the screen's available part), pages narrow and
    // scrolling, OK/Cancel in sight
    void fitScreen(const QRect& room);

    AiSettings aiSettings() const;
    QString language() const; // code, as I18n::language()
    double uiScale() const;   // interface size (UiScale::saved: 0 = the system's)
    Touch::Mode touchMode() const; // larger controls for fingers (Touch::savedMode)
    // The layout form (MainWindow::wantedForm): "" automatic, "phone", "tablet", "desktop" (setting ui/layoutForm)
    QString phoneLayout() const;
    static QString savedPhoneLayout();
    static void setSavedPhoneLayout(const QString& value);
    bool reduceMotion() const; // no fades when the window's arrangement changes (setting ui/reduceMotion)
    QString centerPlayhead() const; // "" automatic (phone, tablet), "on", "off" (setting timeline/centerPlayhead)
    bool magneticMain() const;      // phone/tablet: V1 without gaps (timeline/magneticMain, default on)
    QString clipDrag() const;       // phone/tablet: "hold" (default) or "instant" (timeline/clipDrag)
    QString timelineMomentum() const; // the glide after a flick: "low" (default), "normal", "off" (timeline/momentum)
    bool updateHint() const;  // look for new versions at the start (UpdateCheck::enabled)
    int backgroundThreads() const; // Background::chosenThreads (0 = automatic)
    QStringList vocabulary() const; // the word list for speech recognition (Vocabulary::stored), one entry per line
    bool gameTerms() const;         // a known game's terms for short/direct captions (Vocabulary::gameTermsOn)
    bool captionsCheck() const;     // automatic captions checked by a second model (Vocabulary::captionsCheckOn)
    // Where "Save as" suggests a new project: off = next to its first clip; on = this folder (the user's choice)
    bool autoLinkNodes() const; // Compositing: new nodes linked at once (CompUi::autoLink)
    ZoomAnchor::Mode zoomAnchor() const; // Timeline tab: where wheel zoom stays put (ZoomAnchor::mode)
    PlayheadMove::Mode afterPlayheadMove() const; // Timeline tab: playback after moving the playhead (PlayheadMove::mode)
    // Preview on a second monitor: the main window keeps the viewer's area or closes it up (MainWindow)
    SecondScreenPlan::Mode secondScreenMode() const;
    // Preview resolution of the main window resp. the second monitor; setPreviewProject: fixed sizes above this
    // project's are shown disabled
    void setPreviewProject(const QSize& project);
    PreviewResolution::Choice previewResolution(PreviewResolution::Target target) const;
    // Proxy media made right on import (ProxyManager::importRule)
    ProxyManager::ImportRule proxyImportRule() const;
    // Size of proxy media (ProxyManager::quality)
    ProxyManager::Quality proxyQuality() const;
    // Sound output device (Engine::audioOutput id, empty = system default)
    QString audioOutput() const;
    bool ownProjectFolder() const;
    QString projectFolder() const;
    // The stored choice (QSettings "project/ownFolder", "project/folder"); folder() = the default when none is stored
    static bool storedOwnFolder();
    static QString storedFolder();
    static void store(bool own, const QString& folder);
    // How often the backup copy of the open project is written (QSettings "project/autosaveSeconds", 60 by default:
    // the minute schneidi always had; the default is not stored)
    int autosaveSeconds() const;
    static constexpr int kDefaultAutosaveSeconds = 60;
    static int storedAutosaveSeconds();
    static void setStoredAutosaveSeconds(int seconds);
    // The start page (General › Interface): the pages to choose from as (id, shown name) in bar order, the chosen id
    void setStartPages(const QVector<QPair<QString, QString>>& pages, const QString& current);
    QString startPage() const;
    AiSettingsPage* aiPage() const { return m_aiPage; }
    StartTemplatesPage* startTemplatesPage() const { return m_templatesPage; }
    TimelineSettingsPage* timelinePage() const { return m_timelinePage; }

signals:
    // Buttons for the dialogs that already exist (the caller opens them, they take effect like from the menu)
    void designRequested();
    void keyBindingsRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    QWidget* buildGeneral();
    QRect m_fitRoom; // fitScreen

    QTabWidget* m_tabs = nullptr;
    QWidget* m_generalTab = nullptr;
    QWidget* m_templatesTab = nullptr;
    QWidget* m_timelineTab = nullptr;
    TimelineSettingsPage* m_timelinePage = nullptr;
    QWidget* m_aiTab = nullptr;
    AiSettingsPage* m_aiPage = nullptr;
    StartTemplatesPage* m_templatesPage = nullptr;
    QComboBox* m_language = nullptr;
    QComboBox* m_uiScale = nullptr;
    QComboBox* m_touchMode = nullptr;
    QComboBox* m_phoneLayout = nullptr;
    QCheckBox* m_reduceMotion = nullptr;
    QComboBox* m_centerPlayhead = nullptr;
    QCheckBox* m_magneticMain = nullptr;
    QComboBox* m_clipDrag = nullptr;
    QComboBox* m_momentum = nullptr;
    QComboBox* m_startPage = nullptr;
    QComboBox* m_threads = nullptr;
    QPlainTextEdit* m_vocabulary = nullptr;
    QCheckBox* m_gameTerms = nullptr;
    QCheckBox* m_captionsCheck = nullptr;
    QCheckBox* m_ownFolder = nullptr;
    QComboBox* m_autosave = nullptr;
    QCheckBox* m_autoLink = nullptr;
    QComboBox* m_secondScreen = nullptr;
    QComboBox* m_proxyImport = nullptr;
    QComboBox* m_proxyQuality = nullptr;
    QComboBox* m_audioOutput = nullptr;
    QComboBox* m_viewerResolution = nullptr;
    QComboBox* m_monitorResolution = nullptr;
    QLineEdit* m_folder = nullptr;
    QCheckBox* m_updateHint = nullptr;
    QPushButton* m_checkNow = nullptr;
    QLabel* m_updateResult = nullptr;
    UpdateCheck* m_updates = nullptr;
};
