// MainWindow: import presets for audio streams and ducking (docs/archiv/plan-0.8.md)
#include "app/MainWindow.h"

#include "app/DuckingDialog.h"
#include "app/StreamPresetsDialog.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/StreamPresets.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QApplication>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>

void MainWindow::streamPresetsDialog()
{
    StreamPresetsDialog dlg(m_editor->streamPresets(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    const StreamPresets p = dlg.presets();
    p.save();
    m_editor->setStreamPresets(p);
}

const Clip* MainWindow::streamPresetsClip() const
{
    // First selected audio clip of a file with several titled streams, placed together with its other streams
    const Timeline& tl = m_project->timeline();
    for (int id : m_selection->ids()) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (!c || c->linkId == 0 || c->isTitle()) continue;
        const MediaInfo* m = m_project->mediaInfo(c->mediaPath);
        if (m && m->audioStreamCount() > 1) return c;
    }
    return nullptr;
}

void MainWindow::rememberStreamSettings()
{
    const Clip* c = streamPresetsClip();
    const MediaInfo* m = c ? m_project->mediaInfo(c->mediaPath) : nullptr;
    const QVector<StreamRule> learned = m ? StreamPresets::learn(m_project->timeline(), *m, c->linkId) : QVector<StreamRule>{};
    if (learned.isEmpty()) {
        QApplication::beep(); // no clip of a file with several titled streams selected
        return;
    }
    StreamPresets p = m_editor->streamPresets();
    p.merge(learned);
    p.save();
    m_editor->setStreamPresets(p);
    statusBar()->showMessage(T("Spur-Einstellungen für %1 Streams gemerkt (gilt für neu eingefügte Clips)").arg(learned.size()),
                             5000);
}

void MainWindow::duckingDialog()
{
    DuckingDialog dlg(m_editor, m_editor->selectedAudioClips(), this);
    if (dlg.exec() != QDialog::Accepted) return;
    // Measure where the chosen tracks have sound (mixes them like the export; originals, never proxies)
    QProgressDialog progress(T("Ton wird gemessen…"), T("Abbrechen"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    const auto step = [&](double f) {
        progress.setValue(int(f * 1000));
        QApplication::processEvents();
        return !progress.wasCanceled();
    };
    const Ducking::Params params = dlg.params();
    const auto ranges = AudioAnalysis::soundRanges(m_project->format(), m_project->renderTimeline(), dlg.underTracks(),
                                                   params, step);
    if (progress.wasCanceled()) return;
    progress.setValue(1000);
    if (!ranges) {
        QMessageBox::warning(this, T("Ducking"), T("Der Ton der Spuren ließ sich nicht messen."));
        return;
    }
    if (ranges->isEmpty()) {
        QMessageBox::information(this, T("Ducking"), T("Auf den gewählten Spuren ist kein Ton über der Schwelle."));
        return;
    }
    const int ducked = m_editor->duck(dlg.musicIds(), *ranges, params);
    statusBar()->showMessage(T("Ducking: %1 Clips abgesenkt").arg(ducked), 5000);
}
