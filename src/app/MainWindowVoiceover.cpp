// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: voice-over recording (app/VoiceoverRecorder.h) - the recorder, its menu (input, pre-roll, offset), the
// mic buttons in the timeline's audio track headers (arm, right-click = input), the record button in the timeline's
// toolbar and in the Mixer, the growing take in the timeline, the sign in the viewer, Esc to stop
#include "app/MainWindow.h"
#include "app/VoiceoverRecorder.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "core/Voiceover.h"
#include "engine/Engine.h"
#include "engine/audio/Capture.h"
#include "ui/MediaPool.h"
#include "ui/Mixer.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QActionGroup>
#include <functional>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QShortcut>
#include <QStatusBar>
#include <QUrl>

namespace {

// The input devices (like the output): read when the menu opens, devices come and go. changed: after a choice
void fillInputMenu(QMenu* menu, const std::function<void()>& changed)
{
    menu->clear();
    auto* group = new QActionGroup(menu);
    const QString current = Voiceover::Settings::load().input;
    auto add = [&](const QString& id, const QString& text) {
        QAction* a = menu->addAction(text);
        a->setCheckable(true);
        a->setChecked(id == current);
        group->addAction(a);
        QObject::connect(a, &QAction::triggered, menu, [id, changed] {
            Voiceover::Settings s = Voiceover::Settings::load();
            s.input = id;
            s.save();
            if (changed) changed();
        });
    };
    add(QString(), T("Systemstandard"));
    menu->addSeparator();
    bool found = current.isEmpty();
    for (const Audio::Device::Output& o : Audio::Capture::inputs(Engine::audioDriver())) {
        add(o.id, o.name);
        found = found || o.id == current;
    }
    if (!found) add(current, T("%1 (nicht angeschlossen)").arg(current));
}

} // namespace

void MainWindow::setupVoiceover()
{
    m_voiceover = new VoiceoverRecorder(
        m_engine, m_project, m_editor, [this] { return m_projectPath; },
        [this](const QString& path) {
            if (!m_project->mediaInfo(path)) m_mediaPool->importFiles({path});
            return m_project->mediaInfo(path) != nullptr;
        },
        this);
    m_mixer->setRecorder(m_voiceover);
    // The timeline: mic buttons in the audio track headers, the growing take, the input level of the armed track
    TimelineView* view = m_timeline->view();
    connect(view, &TimelineView::armToggled, this, [this](int track) {
        m_voiceover->setArmedTrack(m_voiceover->armedTrack() == track ? -1 : track);
    });
    connect(view, &TimelineView::armMenuRequested, this, [this](int track, const QPoint& globalPos) {
        QMenu menu(this);
        QAction* arm = menu.addAction(T("Aufnahmebereit"));
        arm->setCheckable(true);
        arm->setChecked(m_voiceover->armedTrack() == track);
        arm->setEnabled(!m_voiceover->recording());
        connect(arm, &QAction::triggered, this, [this, track](bool on) { m_voiceover->setArmedTrack(on ? track : -1); });
        QMenu* input = menu.addMenu(T("Eingang wählen"));
        fillInputMenu(input, [this] { m_voiceover->inputChanged(); });
        menu.addAction(T("Aufnahmeordner öffnen"), this, [this] {
            const QString dir = Voiceover::folder(m_projectPath);
            QDir().mkpath(dir);
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
        });
        menu.exec(globalPos);
    });
    auto syncView = [this, view] { view->setVoiceoverState(m_voiceover->armedTrack(), m_voiceover->recording()); };
    connect(m_voiceover, &VoiceoverRecorder::armedChanged, this, syncView);
    connect(m_voiceover, &VoiceoverRecorder::inputLevel, view, &TimelineView::setInputLevel);
    connect(m_voiceover, &VoiceoverRecorder::takeChanged, this, [this, view] {
        if (m_voiceover->recording()) view->setLiveTake(&m_voiceover->take(), m_voiceover->liveTrack());
    });
    connect(m_voiceover, &VoiceoverRecorder::recordingChanged, this, [this, view, syncView](bool on) {
        syncView();
        if (!on) view->setLiveTake(nullptr, -1);
        if (m_voiceoverAction) m_voiceoverAction->setChecked(on);
    });
    syncView();
    connect(m_mixer, &Mixer::recordRequested, this, [this] {
        if (!m_voiceover->recording()) toggleVoiceover();
    });
    connect(m_mixer, &Mixer::stopRequested, this, [this] {
        if (m_voiceover->recording()) toggleVoiceover();
    });
    // Esc ends the recording (Space and K too: they stop the playback it runs with)
    m_voiceoverEscape = new QShortcut(QKeySequence(Qt::Key_Escape), this, [this] {
        if (m_voiceover->recording()) toggleVoiceover();
    });
    m_voiceoverEscape->setEnabled(false);
    connect(m_voiceover, &VoiceoverRecorder::recordingChanged, this, [this](bool on) {
        m_viewer->setRecording(on);
        m_voiceoverEscape->setEnabled(on);
        if (m_largeEscape) m_largeEscape->setEnabled(m_large.active && !on); // see setViewerLarge
        if (on)
            statusBar()->showMessage(T("Voiceover: Aufnahme auf %1 läuft – Esc oder Leertaste beendet")
                                         .arg(trackShortName({TrackKind::Audio, m_voiceover->liveTrack()})));
    });
    connect(m_voiceover, &VoiceoverRecorder::finished, this,
            [this](const QString& path, bool placed, const QString& error) {
                if (placed)
                    statusBar()->showMessage(T("Voiceover auf %1 gelegt: %2")
                                                 .arg(trackShortName({TrackKind::Audio, m_voiceover->placedTrack()}),
                                                      QFileInfo(path).fileName()),
                                             8000);
                else if (!error.isEmpty())
                    statusBar()->showMessage(T("Voiceover: %1").arg(error), 8000);
            });
}

void MainWindow::toggleVoiceover()
{
    if (!m_voiceover) return;
    if (m_voiceover->recording()) {
        m_voiceover->stop();
        return;
    }
    QString error;
    if (!m_voiceover->start(&error)) QMessageBox::warning(this, T("Voiceover aufnehmen"), error);
}

void MainWindow::endVoiceover()
{
    if (m_voiceover && m_voiceover->recording()) m_voiceover->stop();
}

void MainWindow::buildVoiceoverActions(QMenu* play, QMenu* audioMenu)
{
    QMenu* inputMenu = audioMenu->addMenu(T("Eingabegerät (Voiceover)"));
    connect(inputMenu, &QMenu::aboutToShow, this, [this, inputMenu] {
        fillInputMenu(inputMenu, [this] {
            if (m_voiceover) m_voiceover->inputChanged();
        });
    });

    play->addSeparator();
    // Records onto the armed track (mic in its header), else the selected clips' audio track, else a new one; also
    // a button in the timeline's toolbar (red while recording)
    m_voiceoverAction = makeAction(play, "voiceover_record", T("Voiceover aufnehmen / beenden"), QKeySequence("Shift+R"),
                                   [this] {
                                       toggleVoiceover();
                                       m_voiceoverAction->setChecked(m_voiceover && m_voiceover->recording());
                                   });
    m_voiceoverAction->setCheckable(true);
    m_voiceoverAction->setToolTip(T("Voiceover aufnehmen: auf der Spur mit dem Mikrofon (sonst der gewählten "
                                    "Tonspur oder einer neuen) ab dem Abspielkopf; die Timeline spielt mit"));
    m_timeline->addToolAction(m_voiceoverAction, TimelinePanel::Icon::Record);
    QMenu* options = play->addMenu(T("Voiceover-Optionen"));
    QMenu* preroll = options->addMenu(T("Vorlauf"));
    auto* prerollGroup = new QActionGroup(preroll);
    for (const int seconds : {0, 1, 2, 3, 5}) {
        QAction* a = preroll->addAction(seconds ? T("%1 s").arg(seconds) : T("Aus"));
        a->setCheckable(true);
        a->setData(seconds);
        prerollGroup->addAction(a);
        connect(a, &QAction::triggered, this, [seconds] {
            Voiceover::Settings s = Voiceover::Settings::load();
            s.prerollSeconds = seconds;
            s.save();
        });
    }
    connect(preroll, &QMenu::aboutToShow, this, [prerollGroup] {
        const int current = Voiceover::Settings::load().prerollSeconds;
        for (QAction* a : prerollGroup->actions()) a->setChecked(a->data().toInt() == current);
    });
    options->addAction(T("Latenz-Ausgleich…"), this, [this] {
        Voiceover::Settings s = Voiceover::Settings::load();
        bool ok = false;
        const int ms = QInputDialog::getInt(this, T("Latenz-Ausgleich"),
                                            T("Zusätzlich abziehen (ms; positiv = Stimme lag zu spät):"),
                                            s.offsetMs, -2000, 2000, 1, &ok);
        if (!ok) return;
        s.offsetMs = ms;
        s.save();
    });
    options->addAction(T("Aufnahmeordner öffnen"), this, [this] {
        const QString dir = Voiceover::folder(m_projectPath);
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
}
