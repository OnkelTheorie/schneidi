// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: Auto-Reframe (core/Reframe.h, engine/ReframeAnalysis.h) - the clip/timeline menu entry and the Media
// Pool's "copy as 9:16 timeline". The measuring runs in a thread behind a progress dialog; the change is one undo step.
#include "app/MainWindow.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ReframeAnalysis.h"
#include "ui/timeline/TimelinePanel.h"

#include <QEventLoop>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QUndoStack>

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

} // namespace

bool MainWindow::measureReframe(const QVector<Clip>& clips, const ProjectFormat& format,
                                QHash<int, QVector<Reframe::Key>>* paths)
{
    QHash<QString, int> lengths; // read here: the project is not touched from the thread
    for (const Clip& c : clips)
        if (const MediaInfo* m = m_project->mediaInfo(c.mediaPath)) lengths.insert(c.mediaPath, m->length);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    auto progress = std::make_shared<std::atomic<int>>(0);
    auto result = std::make_shared<QHash<int, QVector<Reframe::Key>>>();
    auto errors = std::make_shared<QStringList>();
    QThread* thread = QThread::create([=] {
        *result = ReframeAnalysis::paths(clips, format, 0.5, [lengths](const QString& p) { return lengths.value(p); },
                                         errors.get(), cancel.get(), progress.get());
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
    return true;
}

void MainWindow::autoReframe()
{
    const QVector<int> ids = m_editor->reframeTargets(m_editor->clipIdsOf(m_selection->ids()));
    if (ids.isEmpty()) {
        statusBar()->showMessage(T("Auto-Reframe: keine Video-Clips (Titel, Compound- und Multicam-Clips bleiben, wie sie sind)"), 6000);
        return;
    }
    QVector<Clip> clips;
    for (int id : ids)
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id)) clips << *c;
    const int sequence = m_project->currentSequence();
    QHash<int, QVector<Reframe::Key>> paths;
    if (!measureReframe(clips, m_project->timelineFormat(), &paths)) return;
    // Another timeline opened or the clips changed meanwhile
    if (m_project->currentSequence() != sequence || footageChanged(m_project->timeline(), clips)) {
        statusBar()->showMessage(T("Auto-Reframe abgebrochen: die Timeline hat sich während der Analyse geändert"), 6000);
        return;
    }
    const int n = m_editor->reframeClips(ids, Reframe::Mode::Auto, paths);
    statusBar()->showMessage(T("Auto-Reframe: %1 Clips folgen ihrem Motiv").arg(n), 5000);
}

void MainWindow::reframedCopy(int sequenceId)
{
    const Sequence* q = m_project->sequence(sequenceId);
    if (!q || q->compound) return;
    ProjectFormat format = m_project->sequenceFormat(sequenceId);
    format.width = 1080;
    format.height = 1920;
    // The clips the copy will reframe, in track order (the copy has the same order, with new ids)
    QVector<Clip> clips;
    for (const Track& t : q->timeline.video)
        for (const Clip& c : t.clips)
            if (!t.locked && Reframe::canReframe(c)) clips << c;
    QHash<int, QVector<Reframe::Key>> paths;
    if (!clips.isEmpty() && !measureReframe(clips, format, &paths)) return;
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
        m_editor->reframeClips({}, Reframe::Mode::Auto, mapped);
    }
    undo->endMacro();
}
