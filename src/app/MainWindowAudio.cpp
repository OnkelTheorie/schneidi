// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: import presets for audio streams and ducking (docs/archiv/plan-0.8.md)
#include "app/MainWindow.h"

#include "app/DuckingDialog.h"
#include "app/FitMusicDialog.h"
#include "app/ToolErrors.h"
#include "app/StreamPresetsDialog.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/StreamPresets.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>

void MainWindow::streamPresetsDialog()
{
    // What applies here: the project's own presets (e.g. from its start template) or the settings' (all projects)
    StreamPresetsDialog dlg(m_project->effectiveImportPresets(), this);
    dlg.setProjectOnly(m_project->importPresets().has_value());
    if (dlg.exec() != QDialog::Accepted) return;
    const StreamPresets p = dlg.presets();
    if (dlg.projectOnly()) {
        m_project->setImportPresets(p);
    } else {
        p.save();
        m_project->setImportPresets(std::nullopt);
    }
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
    // Remembered for every project (the settings) and, if this one has its own presets, there too
    StreamPresets p = StreamPresets::load();
    p.merge(learned);
    p.save();
    if (std::optional<StreamPresets> own = m_project->importPresets()) {
        own->merge(learned);
        m_project->setImportPresets(own);
    }
    m_editor->setStreamPresets(m_project->effectiveImportPresets());
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

void MainWindow::fitMusicDialog()
{
    // The song: the first selected audio clip of a file
    const Timeline& tl = m_project->timeline();
    const Clip* song = nullptr;
    for (int id : m_editor->selectedAudioClips())
        if (const Clip* c = TimelineOps::findClip(tl, id); c && c->kind == ClipKind::Media) {
            song = c;
            break;
        }
    const MediaInfo* m = song ? m_project->mediaInfo(song->mediaPath) : nullptr;
    if (!song || !m) {
        QMessageBox::information(this, T("Musik auf Länge"), T("Erst den Audioclip der Musik in der Timeline auswählen."));
        return;
    }
    const int songId = song->id, songStart = song->start, songFrames = m->length;
    // Length "timeline" = from the song's start to the end of the other clips (as the op takes it)
    int end = 0;
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (c.id != songId) end = std::max(end, c.end());
    const double fps = m_project->timelineFormat().rate.fps();
    FitMusicDialog dlg(songFrames, end - songStart, tl.markIn, tl.markOut, fps, this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (!m_userRunner) return; // no command layer (a bare library build)
    QJsonObject op = dlg.opFields();
    op["op"] = "fit_music";
    op["clip"] = songId;
    statusBar()->showMessage(T("Musik auf Länge: Song wird analysiert…"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QJsonObject reply =
        m_userRunner("edit", {{"project", "@app"}, {"ops", QJsonArray{op}}}, T("Musik auf Länge"));
    QApplication::restoreOverrideCursor();
    statusBar()->clearMessage();
    if (!reply["ok"].toBool()) {
        const QJsonObject error = reply["error"].toObject();
        const QString code = error["code"].toString();
        const auto secs = [fps](int frames) {
            return T("%1 s").arg(QString::number(frames / fps, 'f', 1).replace('.', I18n::decimalPoint()));
        };
        QMessageBox::warning(this, T("Musik auf Länge"),
                             code == "TOO_SHORT"
                                 ? T("Der Song ist nur %1 lang – kürzer als die gewünschte Länge (%2).")
                                       .arg(secs(songFrames), secs(dlg.targetFrames()))
                                 : ToolErrors::text(code, error["message"].toString()));
        return;
    }
    // What came out (the op's answer is in its notes: fit_music {length_s, cuts …})
    for (const QJsonValue& n : reply["result"]["notes"].toArray())
        if (const QJsonObject r = n.toObject()["fit_music"].toObject(); !r.isEmpty()) {
            statusBar()->showMessage(
                T("Musik auf Länge: %1, %2 Schnitte").arg(T("%1 s").arg(QString::number(r["length_s"].toDouble(), 'f', 2).replace('.', I18n::decimalPoint())))
                    .arg(r["cuts"].toArray().size()),
                6000);
            break;
        }
}

void MainWindow::placeSound(const QByteArray& sound, int frame)
{
    if (!m_userRunner) return; // no command layer (a bare library build)
    QJsonObject op = QJsonDocument::fromJson(sound).object();
    if (op["kind"].toString().isEmpty()) return;
    op["op"] = "sfx";
    op["at"] = frame;
    op["_by_hand"] = true; // where the user put it: no sparing rules (those are for placing on cuts or kills)
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QJsonObject reply = m_userRunner("edit", {{"project", "@app"}, {"ops", QJsonArray{op}}}, T("Klang einfügen"));
    QApplication::restoreOverrideCursor();
    if (!reply["ok"].toBool()) {
        const QJsonObject error = reply["error"].toObject();
        QMessageBox::warning(this, T("Klang einfügen"), ToolErrors::text(error["code"].toString(), error["message"].toString()));
        return;
    }
    for (const QJsonValue& n : reply["result"]["notes"].toArray()) {
        const QJsonObject r = n.toObject()["sfx"].toObject();
        if (r.isEmpty() || !r["placed"].toArray().isEmpty()) continue;
        // Left out by the op's rules: why
        const QString why = r["skipped"].toArray().first()["why"].toString();
        const QString text = why == "too_close"    ? T("zu nah an einem anderen Klang")
                             : why == "overlaps"   ? T("an dieser Stelle liegt schon ein Klang")
                             : why == "per_minute" ? T("schon genug Klänge in dieser Minute")
                             : why == "impact_once" ? T("nur ein Einschlag je Video")
                             : why == "before_start" ? T("der Klang finge vor dem Anfang der Timeline an")
                             : why == "after_end"   ? T("der Klang liefe über das Ende der Timeline hinaus")
                                                    : why;
        statusBar()->showMessage(T("Klang nicht eingefügt: %1").arg(text), 6000);
    }
}
