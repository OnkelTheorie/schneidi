// MainWindow: settings of the app (File › Settings…) and applying them
#include "app/MainWindow.h"

#include "app/AiKeys.h"
#include "app/AiLog.h"
#include "app/AiSettingsPage.h"
#include "app/DesignDialog.h"
#include "app/KeyBindingsDialog.h"
#include "app/SettingsDialog.h"
#include "app/StartTemplatesPage.h"
#include "app/Theme.h"
#include "app/UpdateCheck.h"
#include "core/I18n.h"
#include "core/StartTemplates.h"
#include "engine/Background.h"
#include "engine/Frei0r.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

void MainWindow::settingsDialog(int tab)
{
    SettingsDialog dlg(m_aiSettings, this);
    dlg.showTab(tab == 1 ? SettingsDialog::Tab::Ai : tab == 2 ? SettingsDialog::Tab::Templates : SettingsDialog::Tab::General);
    dlg.aiPage()->setClaudeSetup(m_claudeSetup);
    dlg.aiPage()->addChatSection(m_aiKeys.get());
    connect(&dlg, &SettingsDialog::designRequested, this, [this, &dlg] {
        if (DesignDialog(&dlg).exec() == QDialog::Accepted && Theme::restartNeeded())
            offerRestart(T("Das Design ändert sich nach dem Neustart von schneidi."), T("Jetzt neu starten"), T("Später"));
    });
    connect(&dlg, &SettingsDialog::keyBindingsRequested, this, [&dlg] { KeyBindingsDialog(&dlg).exec(); });
    StartTemplatesPage* templates = dlg.startTemplatesPage();
    connect(templates, &StartTemplatesPage::saveRequested, this, [this, &dlg, templates] {
        bool madeDefault = false;
        const QString id = saveStartTemplate(&dlg, &madeDefault);
        if (!id.isEmpty()) templates->reload(madeDefault ? id : QString());
    });
    if (dlg.exec() != QDialog::Accepted) return;
    // Start templates: the template new projects start with, whether New Project asks (test runs keep the settings)
    if (!m_autosaveDisabled) {
        StartTemplates::setDefaultId(templates->defaultId());
        StartTemplates::setAskOnNew(templates->askOnNew());
    }
    if (QString error; !dlg.aiPage()->applyChatKey(&error))
        QMessageBox::warning(this, T("Einstellungen"), T("Schlüssel nicht gespeichert: %1").arg(error));
    setAiSettings(dlg.aiSettings());
    if (dlg.updateHint() != UpdateCheck::enabled() && !m_autosaveDisabled) UpdateCheck::setEnabled(dlg.updateHint());
    if (dlg.backgroundThreads() != Background::chosenThreads() && !m_autosaveDisabled)
        Background::setChosenThreads(dlg.backgroundThreads()); // takes effect with the next job
    if (dlg.frei0r() != Frei0r::enabled() && !m_autosaveDisabled) Frei0r::setEnabled(dlg.frei0r()); // after a restart
    // Language like from the menu (Workspace › Language): takes effect after a restart; test runs keep the setting
    const QString lang = dlg.language();
    if (lang != I18n::language() && !m_autosaveDisabled) {
        I18n::setLanguage(lang);
        offerRestart(I18n::translate(lang, N_("Die Sprache ändert sich nach dem Neustart von schneidi.")),
                     I18n::translate(lang, N_("Jetzt neu starten")), I18n::translate(lang, N_("Später")));
    }
}

void MainWindow::setAiSettings(const AiSettings& s)
{
    const AiSettings before = m_aiSettings;
    m_aiSettings = s;
    if (!m_autosaveDisabled && s != before) s.save(); // test runs leave the user's settings alone
    applyAiMarkStyle();
    m_aiLog->setLimit(s.logLength);
    if (m_liveIdleLimit && s.releaseMinutes != before.releaseMinutes) m_liveIdleLimit(s.releaseMinutes * 60 * 1000);
    // AI connections switched off/on in the settings ("Disconnect" in the indicator is only for this session)
    if (s.liveAllowed != before.liveAllowed) {
        m_liveRefused = false;
        if (s.liveAllowed) {
            if (m_liveAllow) m_liveAllow();
        } else if (m_liveDisconnect) {
            m_liveDisconnect(liveOffReason());
        }
    }
    updateLiveIndicator();
    updateChatPanel();
}

void MainWindow::applyAiMarkStyle()
{
    const AiSettings& s = m_aiSettings;
    m_timeline->view()->setAiMarkStyle({s.highlight, s.color, s.highlightMs, s.distinguish});
}

// ---------- Update hint ----------

QWidget* MainWindow::buildUpdateBar()
{
    // Unobtrusive: a thin bar under the top bar, nothing modal (like the hint above the picture)
    m_updateBar = new QWidget;
    m_updateBar->setObjectName("update_bar");
    m_updateBar->setAttribute(Qt::WA_StyledBackground);
    m_updateBar->setStyleSheet(QString("#update_bar { background: %1; border-bottom: 1px solid %2; }")
                                   .arg(Theme::mix(Theme::panel, Theme::primary, 0.18).name(), Theme::border.name()));
    auto* lay = new QHBoxLayout(m_updateBar);
    lay->setContentsMargins(10, 2, 6, 2);
    lay->setSpacing(6);
    m_updateText = new QLabel;
    m_updateText->setStyleSheet(QString("color: %1;").arg(Theme::text.name()));
    lay->addWidget(m_updateText, 1);
    const auto button = [lay](const char* name, const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        lay->addWidget(b);
        return b;
    };
    QToolButton* now = button("update_now", T("Jetzt"), T("Release-Seite mit den Downloads öffnen"));
    now->setStyleSheet("QToolButton { font-weight: 600; }");
    QToolButton* later = button("update_later", T("Später"), T("Morgen wieder fragen"));
    QToolButton* never = button("update_never", T("Nicht mehr fragen"),
                                T("Nicht mehr nach Updates suchen (wieder einschalten: Einstellungen › Allgemein)"));
    connect(now, &QToolButton::clicked, this, [this] {
        QDesktopServices::openUrl(m_updatePage);
        m_updateBar->hide();
    });
    connect(later, &QToolButton::clicked, m_updateBar, &QWidget::hide); // the daily check asks again
    connect(never, &QToolButton::clicked, this, [this] {
        if (!m_autosaveDisabled) UpdateCheck::setEnabled(false); // test runs leave the setting alone
        m_updateBar->hide();
        statusBar()->showMessage(T("Update-Hinweis aus. Wieder einschalten: Datei › Einstellungen › Allgemein."), 8000);
    });
    m_updateBar->hide();

    m_updates = new UpdateCheck(this);
    connect(m_updates, &UpdateCheck::newer, this, [this](const UpdateCheck::Release& r, const QString& current) {
        m_updatePage = r.page;
        m_updateText->setText(T("schneidi %1 ist da (du hast %2).").arg(r.version, current));
        m_updateBar->show();
    });
    return m_updateBar;
}

void MainWindow::startUpdateCheck()
{
    if (!UpdateCheck::allowedHere() || !UpdateCheck::enabled() || !UpdateCheck::due()) return;
    // A few seconds after the start: opening the project and the first picture come first
    QTimer::singleShot(5000, this, [this] { checkForUpdates(false); });
}

void MainWindow::checkForUpdates(bool manual)
{
    if (m_updates->running()) return;
    // The answers of this check only (an earlier manual one must not open its message box again)
    auto* once = new QObject(this);
    const auto message = [this](QMessageBox::Icon icon, const QString& text) {
        auto* box = new QMessageBox(icon, T("Nach Updates suchen"), text, QMessageBox::Ok, this);
        box->setObjectName("update_message");
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open(); // not exec(): nothing waits on it
    };
    connect(m_updates, &UpdateCheck::newer, once, [this, manual, once] {
        if (!manual && !m_autosaveDisabled) UpdateCheck::markChecked();
        once->deleteLater();
    });
    connect(m_updates, &UpdateCheck::upToDate, once, [this, manual, once, message](const QString& current) {
        if (!manual && !m_autosaveDisabled) UpdateCheck::markChecked();
        if (manual) message(QMessageBox::Information, T("schneidi ist aktuell (%1).").arg(current));
        once->deleteLater();
    });
    // Automatic: silent (no network is normal), the next start tries again
    connect(m_updates, &UpdateCheck::failed, once, [manual, once, message](const QString& error) {
        if (manual) message(QMessageBox::Warning, T("Suche fehlgeschlagen: %1").arg(error));
        once->deleteLater();
    });
    m_updates->check(UpdateCheck::runningVersion(QCoreApplication::applicationVersion()));
}
