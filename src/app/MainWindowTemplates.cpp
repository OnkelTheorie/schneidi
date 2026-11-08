// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: content templates (core/TemplateLibrary.h) – browser, save as template, placeholders, check fields
#include "app/MainWindow.h"

#include "app/TemplateDialogs.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/PathKey.h"
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
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QSet>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>

#include <algorithm>

bool MainWindow::newProjectFromContentTemplate(const ContentTemplate& t)
{
    if (!maybeLeaveProject()) return false;
    ProjectData data;
    QString error;
    if (!TemplateLibrary::load(t, &data, &error)) {
        QMessageBox::warning(this, T("Neu aus Vorlage wählen"), T("Die Vorlage „%1“ ließ sich nicht öffnen:\n%2").arg(t.name, error));
        return false;
    }
    // A new project like a copy: no template of its own, unsaved (saving asks where). Until then it uses the
    // template's files where they are; saving copies them next to the project (copyTemplateFiles)
    data.templateInfo.reset();
    if (!applyLoaded(std::move(data), {})) return false;
    m_templateDir = t.dir;
    removeAutosave();
    statusBar()->showMessage(T("Neues Projekt aus „%1“: Dateien aus dem Medienpool auf die Platzhalter ziehen").arg(t.name), 8000);
    return true;
}

bool MainWindow::copyTemplateFiles(ProjectData& data, const QString& path, QStringList* written)
{
    // The template it was made from, and every template folder it still points into: the link to the template is
    // lost when the never saved project comes back from its automatic backup (or any other way of loading it)
    QStringList dirs = TemplateLibrary::foldersUsed(data);
    if (!m_templateDir.isEmpty()) dirs << QDir::cleanPath(QDir(m_templateDir).absolutePath());
    QSet<QString> seen; // one folder in two spellings (Windows: any case) is one
    dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [&](const QString& d) {
                   const QString key = PathKey::of(d);
                   if (seen.contains(key)) return true;
                   seen.insert(key);
                   return false;
               }),
               dirs.end());
    const QFileInfo fi(path);
    const QString saved = QDir::cleanPath(fi.absoluteFilePath());
    // Saving a template itself (Edit): its files stay in its folder
    dirs.erase(std::remove_if(dirs.begin(), dirs.end(), [&](const QString& d) { return PathKey::isInside(saved, d); }),
               dirs.end());
    if (written) written->clear();
    if (dirs.isEmpty()) return true;
    const QString target = fi.absoluteDir().filePath(fi.completeBaseName() + " media");
    QStringList all;
    for (const QString& dir : std::as_const(dirs)) {
        TemplateLibrary::FileCopies copies;
        QString error;
        // The Media Pool's files of the template too: the user may still put them on the timeline
        if (!TemplateLibrary::copyTemplateFiles(data, dir, target, &copies, &error, &m_templateCopies, true)) {
            for (const QString& f : std::as_const(all)) QFile::remove(f);
            QMessageBox::warning(this, T("Projekt speichern"),
                                 T("Die Dateien der Vorlage ließen sich nicht neben das Projekt kopieren:\n%1").arg(error));
            return false;
        }
        all << copies.written;
    }
    if (written) *written = all;
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
    // A frame about 1.5 s into a preview video for the tiles (Engine::thumbnail without an engine instance): the first
    // one is black when the template fades in; a preview shorter than that shows its first frame
    const ProjectFormat format = m_project->format();
    TemplateBrowserDialog dlg(this, [format](const QString& video, const QSize& size) {
        const QImage later = Engine::thumbnail(format, video, 45, size, true);
        return later.isNull() ? Engine::thumbnail(format, video, 0, size, true) : later;
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
        if (QMessageBox::question(this, T("Projekt als Vorlage mit Platzhaltern speichern"), T("Eine Vorlage „%1“ gibt es schon. Ersetzen?").arg(dlg.name()))
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
    o.lockWaitMs = TemplateLibrary::AppWaitMs; // not a frozen window while an AI's batch holds the library
    QString error, code;
    const std::optional<ContentTemplate> saved = TemplateLibrary::save(data, dlg.name(), o, &error, &code);
    if (!saved) {
        QMessageBox::warning(this, T("Projekt als Vorlage mit Platzhaltern speichern"),
                             code == "BUSY" ? TemplateBrowserDialog::busyText()
                                            : T("Die Vorlage ließ sich nicht speichern:\n%1").arg(error));
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
