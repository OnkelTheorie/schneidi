// MainWindow: multicam like DaVinci (docs/plan-0.8.md) – viewer mode with the grid of the angles, cut/switch keys,
// "Create Multicam Clip" from the Media Pool, angle menu in the timeline.
#include "app/MainWindow.h"

#include "app/InputBindings.h"
#include "app/MulticamDialog.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "engine/SoundSync.h"
#include "engine/TimelineBuilder.h"
#include "ui/MediaPool.h"
#include "ui/MulticamView.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSet>
#include <QSignalBlocker>

namespace {
constexpr int kAngleKeys = 9; // keys 1-9 like DaVinci
constexpr int kTargetTracks = 8;
} // namespace

void MainWindow::buildMulticamViewerAction(QMenu* playback)
{
    // Like DaVinci (View → Viewer Mode → Multicam): the viewer shows the grid of the multicam clip at the playhead
    m_multicamAction = makeAction(playback, "viewer_multicam", T("Multicam-Ansicht"), QKeySequence(),
                                  [this] { setMulticamMode(m_multicamAction->isChecked()); });
    m_multicamAction->setCheckable(true);
    m_multicamAction->setToolTip(T("Viewer zeigt alle Winkel des Multicam-Clips am Playhead. Klick: während der Wiedergabe "
                                   "Schnitt, im Stand Winkel wechseln (Alt+Klick: Schnitt). 1–9 schneidet, Alt+1–9 "
                                   "wechselt den Winkel."));
}

void MainWindow::buildMulticamActions(QMenu* timeline)
{
    QMenu* menu = timeline->addMenu(T("Multicam"));
    makeAction(menu, "multicam_create", T("Multicam-Clip aus ausgewählten Clips erstellen…"), QKeySequence(), [this] {
        const QStringList paths = m_mediaPool->selectedMedia();
        if (paths.isEmpty()) {
            QMessageBox::information(this, T("Neuer Multicam-Clip"),
                                     T("Erst die Aufnahmen (eine je Winkel) im Media Pool auswählen."));
            return;
        }
        createMulticamClip(paths);
    });
    menu->addSeparator();
    // Keys 1-9: angle N of the grid's page; only in the multicam view (setMulticamMode enables them)
    QMenu* cuts = menu->addMenu(T("Schnitt zu Winkel"));
    QMenu* switches = menu->addMenu(T("Winkel wechseln"));
    for (int n = 1; n <= kAngleKeys; ++n) {
        QAction* cut = makeAction(cuts, QString("multicam_cut_%1").arg(n), T("Schnitt zu Winkel %1").arg(n),
                                  QKeySequence(QString::number(n)), [this, n] {
                                      m_multicam->cutToAngle(m_multicam->page() * m_multicam->perPage() + n - 1);
                                  });
        QAction* sw = makeAction(switches, QString("multicam_switch_%1").arg(n), T("Zu Winkel %1 wechseln").arg(n),
                                 QKeySequence(QString("Alt+%1").arg(n)), [this, n] {
                                     m_multicam->switchToAngle(m_multicam->page() * m_multicam->perPage() + n - 1);
                                 });
        cut->setEnabled(false);
        sw->setEnabled(false);
    }
    menu->addSeparator();
    makeAction(menu, "multicam_flatten", T("Multicam-Clip glätten"), QKeySequence(), [this] {
        // Like DaVinci "Flatten Multicam Clip": the selected segments, without a selection the one at the playhead
        QVector<int> ids;
        for (int id : m_editor->clipIdsOf(m_selection->ids()))
            if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && c->isMulticam()) ids << id;
        if (ids.isEmpty())
            if (const Clip* c = Multicam::clipAt(m_project->timeline(), m_timeline->view()->playhead())) ids << c->id;
        if (ids.isEmpty() || !m_editor->flattenMulticamClips(ids)) QApplication::beep();
    });
}

void MainWindow::setupMulticam()
{
    TimelineView* tv = m_timeline->view();
    connect(m_multicam, &MulticamView::gridChanged, this, &MainWindow::syncMulticam);
    connect(m_multicam, &MulticamView::soundAngleChanged, m_engine, &Engine::setMulticamAudio);
    connect(m_engine, &Engine::positionChanged, this, [this](int f) {
        if (m_multicamMode && m_engine->timelinePositions()) m_multicam->setPosition(f);
    });
    connect(m_engine, &Engine::speedChanged, this, [this](double s) { m_multicam->setPlaying(s != 0.0); });
    connect(m_engine, &Engine::frameReady, this, [this](const QImage& img) {
        if (m_engine->mode() == Engine::Mode::Multicam) m_multicam->setFrame(img);
    });
    connect(m_engine, &Engine::modeChanged, this, [this, tv](Engine::Mode mode) {
        m_viewer->showMulticam(m_multicamMode && mode != Engine::Mode::Source);
        if (m_multicamMode && mode == Engine::Mode::Timeline) { // back from the source view (Q)
            m_multicam->setPosition(tv->playhead());
            syncMulticam();
        }
    });
    connect(m_mediaPool, &MediaPool::multicamRequested, this, &MainWindow::createMulticamClip);
    // Proxy hint in the grid: follows the multicam clip and the proxy queue
    ProxyManager* proxies = m_engine->proxies();
    connect(m_multicam, &MulticamView::gridChanged, this, &MainWindow::updateMulticamProxies);
    connect(m_multicam, &MulticamView::proxiesRequested, proxies, &ProxyManager::generate);
    connect(proxies, &ProxyManager::queueChanged, this, &MainWindow::updateMulticamProxies);
    connect(proxies, &ProxyManager::proxyChanged, this, &MainWindow::updateMulticamProxies);
    connect(proxies, &ProxyManager::enabledChanged, this, &MainWindow::updateMulticamProxies);
    updateMulticamProxies();
}

void MainWindow::updateMulticamProxies()
{
    const ProxyManager* proxies = m_engine->proxies();
    QStringList missing;
    int pending = 0;
    if (proxies->enabled()) // proxy media switched off: the user wants the originals
        for (const QString& f : m_multicam->angleFiles()) {
            if (proxies->isPending(f)) ++pending;
            else if (!proxies->hasProxy(f)) missing << f;
        }
    m_multicam->setProxyState(missing, pending);
}

void MainWindow::setMulticamMode(bool on)
{
    m_multicamMode = on;
    if (m_multicamAction && m_multicamAction->isChecked() != on) {
        const QSignalBlocker block(m_multicamAction);
        m_multicamAction->setChecked(on);
    }
    // Number keys only here; Alt+N is the video target track outside (otherwise the shortcut would be ambiguous)
    InputBindings& in = InputBindings::instance();
    for (int n = 1; n <= kAngleKeys; ++n)
        for (const QString& id : {QString("multicam_cut_%1").arg(n), QString("multicam_switch_%1").arg(n)})
            if (QAction* a = in.action(id)) a->setEnabled(on);
    for (int n = 1; n <= kTargetTracks; ++n)
        if (QAction* a = in.action(QString("target_video_%1").arg(n))) a->setEnabled(!on);

    const int pos = m_timeline->view()->playhead();
    if (on) {
        m_multicam->setPlaying(m_engine->speed() != 0.0);
        m_multicam->setPosition(pos);
        if (m_engine->mode() == Engine::Mode::Source) m_engine->showTimeline(pos); // the grid instead of the source
        m_viewer->showMulticam(true);
        syncMulticam();
    } else {
        m_viewer->showMulticam(false);
        if (m_engine->mode() == Engine::Mode::Multicam) m_engine->showTimeline(pos, true);
    }
}

void MainWindow::syncMulticam()
{
    if (m_multicamSyncing) { // e.g. the engine reported its position while switching
        m_multicamSyncAgain = true;
        return;
    }
    if (!m_multicamMode || m_engine->mode() == Engine::Mode::Source) return;
    m_multicamSyncing = true;
    for (int round = 0; round < 3; ++round) {
        m_multicamSyncAgain = false;
        const int pos = m_multicam->position();
        const Sequence* s = m_project->sequence(m_multicam->sequenceId());
        bool shown = false;
        if (s) {
            MulticamGridOptions o;
            o.columns = m_multicam->columns();
            o.page = m_multicam->page();
            o.activeAngle = m_multicam->soundAngle();
            shown = m_engine->showMulticam(m_project->renderTimeline(s->id), s->angles, o, m_multicam->sequenceOffset(),
                                           pos);
        }
        // No multicam clip at the playhead: the timeline plays on (the grid shows the hint)
        if (!shown && m_engine->mode() == Engine::Mode::Multicam) m_engine->showTimeline(pos, true);
        if (!m_multicamSyncAgain) break;
    }
    m_multicamSyncing = false;
}

int MainWindow::selectedMulticamSequence() const
{
    for (int id : m_selection->ids())
        if (const Clip* c = TimelineOps::findClip(m_project->timeline(), id); c && c->isMulticam())
            return c->sequenceId;
    return 0;
}

void MainWindow::addMulticamMenu(QMenu* menu)
{
    const Sequence* s = m_project->sequence(selectedMulticamSequence());
    if (!s) return;
    QVector<int> ids;
    int current = -1;
    for (int id : m_editor->clipIdsOf(m_selection->ids())) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(m_project->timeline(), id, &ref);
        if (!c || !c->isMulticam() || c->sequenceId != s->id) continue;
        ids << id;
        if (current < 0 || ref.kind == TrackKind::Video) current = c->angle;
    }
    // Like DaVinci "Switch Multicam Clip Angle" (mode of the multicam view: picture and/or sound)
    QMenu* sub = menu->addMenu(T("Multicam-Winkel wechseln"));
    const Multicam::Mode mode = m_multicam->mode();
    for (int i = 0; i < s->angles.size(); ++i) {
        QAction* a = sub->addAction(QString("%1  %2").arg(i + 1).arg(s->angles[i].name));
        a->setCheckable(true);
        a->setChecked(i == current);
        connect(a, &QAction::triggered, this, [this, ids, i, mode] { m_editor->setMulticamAngle(ids, i, mode); });
    }
    if (QAction* flatten = InputBindings::instance().action("multicam_flatten")) menu->addAction(flatten);
}

void MainWindow::createMulticamClip(const QStringList& paths)
{
    QStringList files;
    for (const QString& p : paths)
        if (const MediaInfo* m = m_project->mediaInfo(p); m && !m->isImage && (m->hasVideo || m->hasAudio)) files << p;
    if (files.isEmpty()) return;
    QSet<QString> used;
    for (const Sequence& s : m_project->sequences()) used.insert(s.name);
    QString def;
    for (int i = 1;; ++i) {
        def = T("Multicam-Clip %1").arg(i);
        if (!used.contains(def)) break;
    }
    MulticamDialog dlg(m_project, files, def, this);
    if (dlg.exec() != QDialog::Accepted) return;
    m_engine->pause();

    QVector<Multicam::Source> sources;
    for (const QString& f : files) sources << Multicam::Source{{}, {f}};
    QVector<AudioSync::Match> sound;
    if (dlg.method() == Multicam::SyncMethod::Sound) {
        // Decoding hours of sound takes a while: progress + cancel
        QProgressDialog progress(T("Ton wird synchronisiert…"), T("Abbrechen"), 0, 1000, this);
        progress.setWindowTitle(T("Neuer Multicam-Clip"));
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(300);
        QVector<SoundSync::Input> inputs;
        for (const QString& f : files) {
            const MediaInfo* m = m_project->mediaInfo(f);
            inputs << SoundSync::Input{f, dlg.stream() < m->audioStreamCount() ? dlg.stream() : 0};
        }
        sound = SoundSync::align(inputs, {}, [&](double f) {
            progress.setValue(int(f * 1000));
            QApplication::processEvents();
            return !progress.wasCanceled();
        });
        if (progress.wasCanceled() || sound.isEmpty()) return;
        progress.setValue(1000);
    }
    const QVector<Multicam::Angle> angles = Multicam::resolve(*m_project, sources, dlg.method(), sound);
    const int seq = m_editor->createMulticamClip(dlg.name(), angles, m_mediaPool->currentBin());
    if (!seq) {
        QMessageBox::warning(this, T("Neuer Multicam-Clip"), T("Keine der Aufnahmen lässt sich als Winkel verwenden."));
        return;
    }
    QStringList unsynced;
    for (const Multicam::Angle& a : angles)
        for (const Multicam::FilePlacement& f : a.files)
            if (!f.synced) unsynced << QFileInfo(f.path).fileName();
    if (!unsynced.isEmpty())
        QMessageBox::information(this, T("Neuer Multicam-Clip"),
                                 T("Nicht synchron (am Anfang platziert, in der Multicam-Timeline von Hand verschieben "
                                   "oder per Marker synchronisieren):\n%1")
                                     .arg(unsynced.join('\n')));
}
