// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: content templates (core/TemplateLibrary.h) – browser, save as template, placeholders, check fields
#include "app/MainWindow.h"

#include "app/TemplateDialogs.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TemplateFill.h"
#include "core/TemplateLibrary.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/SlotText.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

bool MainWindow::newProjectFromContentTemplate(const ContentTemplate& t)
{
    if (!maybeLeaveProject()) return false;
    ProjectData data;
    QString error;
    if (!TemplateLibrary::load(t, &data, &error)) {
        QMessageBox::warning(this, T("Neu aus Vorlage"), T("Die Vorlage „%1“ ließ sich nicht öffnen:\n%2").arg(t.name, error));
        return false;
    }
    // A new project like a copy: no template of its own, unsaved (saving asks where)
    data.templateInfo.reset();
    if (!applyLoaded(std::move(data), {})) return false;
    removeAutosave();
    statusBar()->showMessage(T("Neues Projekt aus „%1“: Dateien aus dem Media Pool auf die Platzhalter ziehen").arg(t.name), 8000);
    return true;
}

bool MainWindow::editContentTemplate(const ContentTemplate& t)
{
    if (t.builtIn || !maybeLeaveProject()) return false;
    ProjectData data;
    QString error;
    if (!TemplateLibrary::load(t, &data, &error)) {
        QMessageBox::warning(this, T("Vorlage bearbeiten"), T("Die Vorlage „%1“ ließ sich nicht öffnen:\n%2").arg(t.name, error));
        return false;
    }
    if (!applyLoaded(std::move(data), t.project)) return false;
    removeAutosave();
    return true;
}

void MainWindow::templateBrowser()
{
    // First frame of a preview video for the tiles (Engine::thumbnail without an engine instance)
    const ProjectFormat format = m_project->format();
    TemplateBrowserDialog dlg(this, [format](const QString& video, const QSize& size) {
        return Engine::thumbnail(format, video, 0, size, true);
    });
    if (dlg.exec() != QDialog::Accepted) return;
    switch (dlg.choice()) {
    case TemplateBrowserDialog::Choice::NewContent: newProjectFromContentTemplate(dlg.content()); break;
    case TemplateBrowserDialog::Choice::Edit: editContentTemplate(dlg.content()); break;
    case TemplateBrowserDialog::Choice::NewStart: newProjectFromTemplate(dlg.start()); break;
    case TemplateBrowserDialog::Choice::None: break;
    }
}

void MainWindow::saveContentTemplate()
{
    TemplateInfo info = m_project->templateInfo().value_or(TemplateInfo{});
    if (info.name.isEmpty() && !m_projectPath.isEmpty()) info.name = QFileInfo(m_projectPath).completeBaseName();
    SaveTemplateDialog dlg(info, this);
    if (dlg.exec() != QDialog::Accepted) return;
    TemplateLibrary::SaveOptions o;
    o.description = dlg.description();
    o.tags = dlg.tags();
    o.aspect = info.aspect;
    o.copyMedia = dlg.copyMedia();
    const QString id = TemplateLibrary::idForName(dlg.name());
    if (QFileInfo::exists(QDir(TemplateLibrary::dir()).filePath(id))) {
        if (QMessageBox::question(this, T("Als Vorlage speichern"), T("Eine Vorlage „%1“ gibt es schon. Ersetzen?").arg(dlg.name()))
            != QMessageBox::Yes)
            return;
        o.replace = true;
    }
    // Preview: the viewer's picture at the playhead, small (a still; a preview video would need a render)
    QTemporaryDir tmp;
    if (dlg.preview() && tmp.isValid()) {
        const QImage still = m_engine->grabStill(m_project->timeline());
        if (!still.isNull()) {
            const QString png = tmp.filePath("preview.png");
            if (still.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(png)) o.preview = png;
        }
    }
    ProjectData data = m_project->data();
    data.playhead = m_timeline->view()->playhead();
    QString error;
    const std::optional<ContentTemplate> saved = TemplateLibrary::save(data, dlg.name(), o, &error);
    if (!saved) {
        QMessageBox::warning(this, T("Als Vorlage speichern"), T("Die Vorlage ließ sich nicht speichern:\n%1").arg(error));
        return;
    }
    statusBar()->showMessage(T("Vorlage „%1“ gespeichert (%2 Felder)").arg(saved->name).arg(saved->slotIds.size()), 6000);
}

void MainWindow::markPlaceholder()
{
    // The first selected clip that can be a placeholder (video before its linked audio)
    const Timeline& tl = m_project->timeline();
    const QVector<int> ids = m_editor->clipIdsOf(m_selection->ids());
    const Clip* chosen = nullptr;
    SlotKind kind = SlotKind::Clip;
    for (TrackKind want : {TrackKind::Video, TrackKind::Audio})
        for (int id : ids) {
            TrackRef ref;
            const Clip* c = TimelineOps::findClip(tl, id, &ref);
            if (chosen || !c || ref.kind != want) continue;
            for (SlotKind k : {SlotKind::Clip, SlotKind::Text, SlotKind::Music})
                if (TemplateFill::canHold(*c, ref.kind, k).isEmpty()) {
                    chosen = c;
                    kind = k;
                    break;
                }
        }
    if (!chosen) {
        statusBar()->showMessage(T("Platzhalter können Clips einer Datei, Texttitel und Audioclips sein."), 5000);
        return;
    }
    if (chosen->slot.isSet()) { // already one: show its rules
        m_inspectorToggle->setChecked(true);
        return;
    }
    // A free id of its kind: clip1, clip2 …; the label in the user's language
    const QStringList used = TemplateFill::slotIds(m_project->data());
    const QString base = QString::fromLatin1(kSlotKindIds[int(kind)]);
    int n = 1;
    while (used.contains(base + QString::number(n))) ++n;
    ClipSlot slot;
    slot.id = base + QString::number(n);
    slot.label = QString("%1 %2").arg(SlotText::kindName(kind)).arg(n);
    slot.kind = kind;
    if (kind == SlotKind::Text) slot.maxChars = 0;
    if (const QString why = m_editor->setClipSlot(chosen->id, slot); !why.isEmpty()) {
        statusBar()->showMessage(T("Nicht möglich: %1").arg(why), 5000);
        return;
    }
    m_inspectorToggle->setChecked(true); // the Inspector shows the section "Placeholder"
}

void MainWindow::removePlaceholders()
{
    QVector<int> marked;
    for (int id : m_editor->clipIdsOf(m_selection->ids()))
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && c->slot.isSet()) marked << id;
    if (marked.isEmpty()) return;
    QUndoStack* undo = m_project->undoStack();
    if (marked.size() > 1) undo->beginMacro(T("Platzhalter entfernen"));
    for (int id : marked) m_editor->setClipSlot(id, {});
    if (marked.size() > 1) undo->endMacro();
}

void MainWindow::checkPlaceholders()
{
    SlotCheckDialog dlg(m_project->data(), this);
    dlg.exec();
}
