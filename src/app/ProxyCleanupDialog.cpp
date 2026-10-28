// SPDX-License-Identifier: AGPL-3.0-only
#include "app/ProxyCleanupDialog.h"

#include "core/I18n.h"
#include "core/ProjectFile.h"
#include "engine/ProxyCleanup.h"
#include "engine/ProxyManager.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ProxyCleanupDialog {

namespace {

// Originals whose proxies stay: the open project's (unsaved changes included) and the recent projects'
QStringList keepList(const ProjectData& open)
{
    return ProxyCleanup::projectMedia(open) + ProxyCleanup::mediaOfProjects(ProxyCleanup::recentProjects());
}

} // namespace

qint64 run(QWidget* parent, const ProjectData& open, const ProxyManager* proxies)
{
    QApplication::setOverrideCursor(Qt::WaitCursor); // reads the recent projects from disk
    const ProxyCleanup::Plan plan = ProxyCleanup::plan(keepList(open), {proxies->writing()});
    QApplication::restoreOverrideCursor();

    if (plan.items.isEmpty()) {
        QMessageBox box(parent);
        box.setWindowTitle(T("Proxy-Medien aufräumen"));
        box.setIcon(QMessageBox::Information);
        box.setText(plan.kept == 0 ? T("Nichts aufzuräumen: Es gibt keine Proxy-Dateien.")
                                   : T("Nichts aufzuräumen: Jede Proxy-Datei gehört zum offenen oder zu einem zuletzt "
                                       "geöffneten Projekt (zusammen %1).")
                                         .arg(ProxyCleanup::sizeText(plan.keptBytes)));
        box.exec();
        return 0;
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(T("Proxy-Medien aufräumen"));
    auto* layout = new QVBoxLayout(&dialog);

    int unused = 0; // proxies among the items (the rest are leftovers)
    for (const ProxyCleanup::Item& it : plan.items) unused += it.reason != ProxyCleanup::Reason::Leftover;
    auto* summary = new QLabel(
        T("%1 von %2 Proxy-Dateien braucht weder das offene noch ein zuletzt geöffnetes Projekt (zusammen %3).")
            .arg(unused)
            .arg(unused + plan.kept)
            .arg(ProxyCleanup::sizeText(plan.bytes)));
    summary->setObjectName("summary");
    summary->setWordWrap(true);
    layout->addWidget(summary);

    // What goes: the file it was made for (the proxy itself when that is unknown), why, how big
    auto* list = new QTreeWidget;
    list->setObjectName("items");
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus); // a list to read, nothing to pick
    list->setHeaderLabels({T("Datei"), T("Grund"), T("Größe")});
    for (const ProxyCleanup::Item& it : plan.items) {
        auto* row = new QTreeWidgetItem(list, {QFileInfo(it.original.isEmpty() ? it.file : it.original).fileName(),
                                               ProxyCleanup::reasonText(it.reason), ProxyCleanup::sizeText(it.bytes)});
        row->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        row->setToolTip(0, it.original.isEmpty() ? it.file : it.original + "\n→ " + it.file);
    }
    list->header()->setStretchLastSection(false);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    layout->addWidget(list, 1);

    auto* info = new QLabel(T("Gelöscht wird, wessen Original fehlt oder geändert wurde und was zu keinem dieser "
                              "Projekte gehört. Die Originale bleiben unberührt; Proxys lassen sich jederzeit neu "
                              "erzeugen.")
                            + " "
                            + T("Es bleiben %1 Proxy-Dateien (%2).").arg(plan.kept).arg(ProxyCleanup::sizeText(plan.keptBytes)));
    info->setWordWrap(true);
    layout->addWidget(info);

    auto* buttons = new QDialogButtonBox;
    QPushButton* remove =
        buttons->addButton(T("%1 freigeben").arg(ProxyCleanup::sizeText(plan.bytes)), QDialogButtonBox::DestructiveRole);
    remove->setObjectName("free");
    remove->setAutoDefault(false);
    QPushButton* cancel = buttons->addButton(T("Abbrechen"), QDialogButtonBox::RejectRole);
    cancel->setDefault(true);
    QObject::connect(remove, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(640, 420);

    if (dialog.exec() != QDialog::Accepted) return 0;
    // The project may have changed while the dialog was open (an AI session imports): its proxies still stay
    return ProxyCleanup::apply(plan, {proxies->writing()}, keepList(open));
}

} // namespace ProxyCleanupDialog
