#include "app/ProxyCleanupDialog.h"

#include "core/I18n.h"
#include "core/ProjectFile.h"
#include "engine/ProxyCleanup.h"
#include "engine/ProxyManager.h"

#include <QApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>

namespace ProxyCleanupDialog {

qint64 run(QWidget* parent, const ProjectData& open, const ProxyManager* proxies)
{
    QApplication::setOverrideCursor(Qt::WaitCursor); // reads the recent projects from disk
    const QStringList keep =
        ProxyCleanup::projectMedia(open) + ProxyCleanup::mediaOfProjects(ProxyCleanup::recentProjects());
    const ProxyCleanup::Plan plan = ProxyCleanup::plan(keep, {proxies->writing()});
    QApplication::restoreOverrideCursor();

    QMessageBox box(parent);
    box.setWindowTitle(T("Proxy-Medien aufräumen"));
    if (plan.items.isEmpty()) {
        box.setIcon(QMessageBox::Information);
        box.setText(plan.kept == 0 ? T("Nichts aufzuräumen: Es gibt keine Proxy-Dateien.")
                                   : T("Nichts aufzuräumen: Jede Proxy-Datei gehört zum offenen oder zu einem zuletzt "
                                       "geöffneten Projekt (zusammen %1).")
                                         .arg(ProxyCleanup::sizeText(plan.keptBytes)));
        box.exec();
        return 0;
    }
    int unused = 0; // proxies among the items (the rest are leftovers)
    for (const ProxyCleanup::Item& it : plan.items) unused += it.reason != ProxyCleanup::Reason::Leftover;
    box.setIcon(QMessageBox::Question);
    box.setText(T("%1 von %2 Proxy-Dateien braucht weder das offene noch ein zuletzt geöffnetes Projekt (zusammen %3).")
                    .arg(unused)
                    .arg(unused + plan.kept)
                    .arg(ProxyCleanup::sizeText(plan.bytes)));
    box.setInformativeText(T("Gelöscht wird, wessen Original fehlt oder geändert wurde und was zu keinem dieser "
                             "Projekte gehört. Die Originale bleiben unberührt; Proxys lassen sich jederzeit neu "
                             "erzeugen.")
                           + "\n\n"
                           + T("Es bleiben %1 Proxy-Dateien (%2).").arg(plan.kept).arg(ProxyCleanup::sizeText(plan.keptBytes)));
    QStringList lines;
    for (const ProxyCleanup::Item& it : plan.items)
        lines << QString("%1: %2, %3")
                     .arg(QFileInfo(it.original.isEmpty() ? it.file : it.original).fileName(),
                          ProxyCleanup::reasonText(it.reason), ProxyCleanup::sizeText(it.bytes));
    box.setDetailedText(lines.join('\n'));
    QPushButton* remove = box.addButton(T("%1 freigeben").arg(ProxyCleanup::sizeText(plan.bytes)),
                                        QMessageBox::DestructiveRole);
    QPushButton* cancel = box.addButton(T("Abbrechen"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel);
    box.exec();
    if (box.clickedButton() != remove) return 0;
    return ProxyCleanup::apply(plan, {proxies->writing()});
}

} // namespace ProxyCleanupDialog
