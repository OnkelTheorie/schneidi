// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: Auto-Reframe (core/Reframe.h, engine/ReframeAnalysis.h) - the clip/timeline menu entry and the Media
// Pool's "copy as 9:16 timeline". The measuring runs in a thread behind a progress dialog; the change is one undo step.
// Game footage keeps its kill feed readable at the top (Reframe::KeepChoice::Auto) unless switched off in the menu.
#include "app/MainWindow.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/BarDetect.h"
#include "engine/ReframeAnalysis.h"
#include "ui/timeline/TimelinePanel.h"

#include <QApplication>
#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <atomic>
#include <memory>

namespace {

// The part of its file a clip shows: what the measurement depends on
bool sameFootage(const Clip& a, const Clip& b)
{
    return a.mediaPath == b.mediaPath && a.in == b.in && a.out == b.out && a.speed == b.speed && a.reverse == b.reverse
           && a.freeze == b.freeze && a.freezeFrame == b.freezeFrame && a.ramp == b.ramp;
}

// Were the measured clips changed while the dialog was up (live edits keep running)? Then the paths do not fit.
bool footageChanged(const Timeline& tl, const QVector<Clip>& measured)
{
    for (const Clip& c : measured) {
        const Clip* now = TimelineOps::findClip(tl, c.id);
        if (!now || !sameFootage(*now, c)) return true;
    }
    return false;
}

// Status text for the overlays a whole-timeline reframe left alone
QString overlayNote(int n)
{
    return n == 1 ? T("1 Einblendung (kleineres Bild auf einer oberen Spur, z. B. Webcam) bleibt an seinem Platz im Bild.")
                  : T("%1 Einblendungen (kleinere Bilder auf oberen Spuren, z. B. Webcam) bleiben an ihrem Platz im Bild.").arg(n);
}

} // namespace

bool MainWindow::measureReframe(const QVector<Clip>& clips, const ProjectFormat& format,
                                QHash<int, QVector<Reframe::Key>>* paths, QHash<QString, QRectF>* contents)
{
    QHash<QString, int> lengths; // read here: the project is not touched from the thread
    for (const Clip& c : clips)
        if (const MediaInfo* m = m_project->mediaInfo(c.mediaPath)) lengths.insert(c.mediaPath, m->length);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto progress = std::make_shared<std::atomic<int>>(0);
    auto result = std::make_shared<QHash<int, QVector<Reframe::Key>>>();
    auto errors = std::make_shared<QStringList>();
    auto bars = std::make_shared<QHash<QString, QRectF>>(); // black bars are never wanted in a reframed picture
    QThread* thread = QThread::create([=] {
        *result = ReframeAnalysis::paths(clips, format, 0.5, [lengths](const QString& p) { return lengths.value(p); },
                                         errors.get(), cancel.get(), progress.get(), bars.get());
    });
    QProgressDialog dialog(T("Auto-Reframe: Bild wird analysiert …"), T("Abbrechen"), 0, 1000, this);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(400);
    connect(&dialog, &QProgressDialog::canceled, this, [cancel] { cancel->store(true); });
    QEventLoop loop;
    connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    // Another project opened meanwhile (a live edit from schneidi-cli): the measurement is for clips that are gone
    QMetaObject::Connection replaced = connect(m_project, &Project::loaded, &loop, [cancel, &loop] {
        cancel->store(true);
        loop.quit();
    });
    QTimer poll;
    poll.setInterval(100);
    connect(&poll, &QTimer::timeout, &dialog, [&] { dialog.setValue(std::min(999, progress->load())); });
    thread->start();
    poll.start();
    loop.exec();
    disconnect(replaced);
    // The loop also ends when the app quits: stop measuring instead of waiting for it
    if (!thread->isFinished()) cancel->store(true);
    thread->wait();
    delete thread;
    poll.stop();
    dialog.reset();
    if (cancel->load()) return false;
    if (!errors->isEmpty())
        QMessageBox::warning(this, T("Auto-Reframe"),
                             T("Nicht alle Clips ließen sich analysieren, sie bleiben mittig:\n%1").arg(errors->join('\n')));
    *paths = *result;
    if (contents) *contents = *bars;
    return true;
}

void MainWindow::autoReframe()
{
    QVector<int> overlays; // left out of a whole timeline (nothing selected)
    const QVector<int> ids = m_editor->reframeTargets(m_editor->clipIdsOf(m_selection->ids()), &overlays);
    if (ids.isEmpty()) {
        statusBar()->showMessage(T("Auto-Reframe: keine Video-Clips (Titel, Verbund- und Multicam-Clips bleiben, wie sie sind)"), 6000);
        return;
    }
    QVector<Clip> clips;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) clips << *c;
    const int sequence = m_project->currentSequence();
    QHash<int, QVector<Reframe::Key>> paths;
    QHash<QString, QRectF> contents;
    if (!measureReframe(clips, m_project->timelineFormat(), &paths, &contents)) return;
    // Another timeline opened or the clips changed meanwhile
    if (m_project->currentSequence() != sequence || footageChanged(m_project->timeline(), clips)) {
        statusBar()->showMessage(T("Auto-Reframe abgebrochen: die Timeline hat sich während der Analyse geändert"), 6000);
        return;
    }
    const auto keeps = m_editor->reframeKeeps(ids, Reframe::Mode::Auto, contents, reframeKeepChoice());
    const int n = m_editor->reframeClips(ids, Reframe::Mode::Auto, paths, contents, &keeps);
    const bool kept = std::any_of(keeps.begin(), keeps.end(), [](const QVector<Reframe::Keep>& k) { return !k.isEmpty(); });
    QString text = kept ? T("Auto-Reframe: %1 Clips folgen ihrem Motiv, der Killfeed bleibt oben lesbar").arg(n)
                        : T("Auto-Reframe: %1 Clips folgen ihrem Motiv").arg(n);
    if (!overlays.isEmpty()) text += " " + overlayNote(int(overlays.size()));
    statusBar()->showMessage(text, overlays.isEmpty() ? 5000 : 8000);
}

void MainWindow::reframedCopy(int sequenceId)
{
    const Sequence* q = m_project->sequence(sequenceId);
    if (!q || q->compound) return;
    ProjectFormat format = m_project->sequenceFormat(sequenceId);
    format.width = 1080;
    format.height = 1920;
    // The clips the copy will reframe, in track order (the copy has the same order, with new ids)
    // (overlays such as a webcam picture on V2 are not reframed: they keep their place in the frame)
    const QSizeF sourceFrame = m_project->sequenceFormat(sequenceId).size();
    QVector<Clip> clips;
    for (int t = 0; t < q->timeline.video.size(); ++t)
        for (const Clip& c : q->timeline.video[t].clips)
            if (!q->timeline.video[t].locked && Reframe::canReframe(c)
                && !Reframe::isOverlay(c, t, MediaAspect::ofClip(c), sourceFrame))
                clips << c;
    QHash<int, QVector<Reframe::Key>> paths;
    QHash<QString, QRectF> contents;
    if (!clips.isEmpty() && !measureReframe(clips, format, &paths, &contents)) return;
    q = m_project->sequence(sequenceId);
    if (!q || footageChanged(q->timeline, clips)) {
        statusBar()->showMessage(T("Auto-Reframe abgebrochen: die Timeline hat sich während der Analyse geändert"), 6000);
        return;
    }
    const Timeline source = q->timeline;
    const QString name = q->name;

    QUndoStack* undo = m_project->undoStack();
    undo->beginMacro(T("Als Hochformat-Timeline kopieren (9:16)"));
    const int copy = m_project->duplicateSequence(sequenceId);
    if (copy) {
        m_project->renameSequence(copy, name + " 9:16");
        m_project->setSequenceSize(copy, format.size() == m_project->format().size() ? QSize() : format.size());
        m_timeline->openSequence(copy);
        // Old clip id -> new one: the copy keeps tracks and clip order
        QHash<int, QVector<Reframe::Key>> mapped;
        const Timeline& now = m_project->timeline();
        for (int t = 0; t < source.video.size() && t < now.video.size(); ++t)
            for (int i = 0; i < source.video[t].clips.size() && i < now.video[t].clips.size(); ++i)
                if (paths.contains(source.video[t].clips[i].id))
                    mapped.insert(now.video[t].clips[i].id, paths.value(source.video[t].clips[i].id));
        QVector<int> overlays;
        const QVector<int> targets = m_editor->reframeTargets({}, &overlays);
        const auto keeps = m_editor->reframeKeeps(targets, Reframe::Mode::Auto, contents, reframeKeepChoice());
        if (!targets.isEmpty()) m_editor->reframeClips(targets, Reframe::Mode::Auto, mapped, contents, &keeps);
        if (!overlays.isEmpty()) statusBar()->showMessage(overlayNote(int(overlays.size())), 8000);
    }
    undo->endMacro();
}

Reframe::KeepChoice MainWindow::reframeKeepChoice() const
{
    Reframe::KeepChoice c;
    if (!QSettings().value(QStringLiteral("reframe/keepKillFeed"), true).toBool()) c.kind = Reframe::KeepChoice::None;
    return c;
}

void MainWindow::cropBlackBars()
{
    const QVector<int> ids = m_editor->reframeTargets(m_editor->clipIdsOf(m_selection->ids()));
    if (ids.isEmpty()) {
        statusBar()->showMessage(T("Schwarze Balken: keine Video-Clips ausgewählt"), 6000);
        return;
    }
    // A dozen pictures per file, cached: short enough to wait for
    QHash<QString, QRectF> contents;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && !contents.contains(c->mediaPath))
            contents.insert(c->mediaPath, BarDetect::content(c->mediaPath).value_or(QRectF(0, 0, 1, 1)));
    QApplication::restoreOverrideCursor();
    const int n = m_editor->cropBars(ids, contents);
    statusBar()->showMessage(n ? T("Schwarze Balken weggeschnitten: %1 Clips").arg(n)
                               : T("Keine schwarzen Balken gefunden"), 6000);
}

void MainWindow::setStretched43(bool on)
{
    const QVector<int> ids = m_editor->reframeTargets(m_editor->clipIdsOf(m_selection->ids()));
    if (ids.isEmpty()) return;
    m_editor->setPictureAspect(ids, on ? 4.0 / 3.0 : 0.0);
}
