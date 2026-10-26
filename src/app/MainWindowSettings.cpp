// MainWindow: settings of the app (File › Settings…) and applying them
#include "app/MainWindow.h"

#include "app/DesignDialog.h"
#include "app/KeyBindingsDialog.h"
#include "app/SettingsDialog.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

void MainWindow::settingsDialog(int tab)
{
    SettingsDialog dlg(m_aiSettings, this);
    dlg.showTab(tab == 1 ? SettingsDialog::Tab::Ai : SettingsDialog::Tab::General);
    connect(&dlg, &SettingsDialog::designRequested, this, [this, &dlg] {
        if (DesignDialog(&dlg).exec() == QDialog::Accepted && Theme::restartNeeded())
            offerRestart(T("Das Design ändert sich nach dem Neustart von schneidi."), T("Jetzt neu starten"), T("Später"));
    });
    connect(&dlg, &SettingsDialog::keyBindingsRequested, this, [&dlg] { KeyBindingsDialog(&dlg).exec(); });
    if (dlg.exec() != QDialog::Accepted) return;
    setAiSettings(dlg.aiSettings());
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
    m_timeline->view()->setFlashStyle(s.highlight, s.color, s.highlightMs);
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
}
