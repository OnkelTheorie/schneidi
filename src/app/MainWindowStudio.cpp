// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the Studio page (0.23) - built lazily on its first showing, follows the playhead, its clicks seek like the
// timeline's; its tools (Block 2) record through the app's voice-over recorder, run commands as the user's own undo
// step and preview / place effect sounds like the Effects Library.
#include "app/MainWindow.h"

#include "app/VoiceoverRecorder.h"
#include "engine/Engine.h"
#include "ui/studio/StudioMixer.h"
#include "ui/studio/StudioPage.h"
#include "ui/studio/StudioTools.h"
#include "ui/studio/StudioTracks.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>

void MainWindow::ensureStudioPage()
{
    if (m_studioPage) return;
    m_studioPage = new StudioPage(m_editor, m_engine, m_mediaCache);
    m_pages->addWidget(m_studioPage);
    // New names since the layout of 0.23 (lanes | right column): the old sizes ("studioRoot", "studioTop") belonged
    // to other splitters
    watchSizes(m_studioPage->root(), "studioMain");
    watchSizes(m_studioPage->side(), "studioSide");
    watchSizes(m_studioPage->left(), "studioLeft");
    TimelineView* tv = m_timeline->view();
    StudioTracks* tracks = m_studioPage->tracks();
    // The lanes start where the timeline is (its zoom and scroll, once); from then on they are the user's own
    tracks->setView(tv->view().pxPerFrame, tv->view().leftFrame);
    tracks->setPlayhead(tv->playhead());
    // The timeline's playhead while the viewer shows a file (Source mode): the mixer's read-mode faders stay there
    m_studioPage->mixer()->setTimelinePlayhead([tv] { return tv->playhead(); });
    connect(m_engine, &Engine::positionChanged, tracks, [this, tracks](int f) {
        if (m_engine->timelinePositions()) tracks->setPlayhead(f);
    });
    connect(tracks, &StudioTracks::seekRequested, tv, &TimelineView::seekRequested);
    connect(tracks, &StudioTracks::playheadMoveStarted, tv, &TimelineView::playheadMoveStarted);
    connect(tracks, &StudioTracks::playheadMoveFinished, tv, &TimelineView::playheadMoveFinished);

    StudioTools* tools = m_studioPage->tools();
    tools->setRunner([this](const QString& command, const QJsonObject& args, const QString& undoText) {
        return m_userRunner ? m_userRunner(command, args, undoText) : QJsonObject{};
    });
    if (m_voiceover) {
        tools->setRecorder(m_voiceover);
        // The growing take on its lane, as in the timeline
        connect(m_voiceover, &VoiceoverRecorder::takeChanged, tracks, [this, tracks] {
            if (m_voiceover->live()) tracks->setLiveTake(&m_voiceover->take(), m_voiceover->liveTrack());
        });
        connect(m_voiceover, &VoiceoverRecorder::recordingChanged, tracks, [tracks](bool on) {
            if (!on) tracks->setLiveTake(nullptr, -1);
        });
    }
    connect(tools, &StudioTools::recordRequested, this, &MainWindow::toggleVoiceover);
    // Effect sounds: heard in the player like a double-click in the Effects Library, placed through op 'sfx'
    connect(tools, &StudioTools::soundPreviewRequested, this, [this](const QString& file) {
        m_sourcePos.remove(file);
        previewFile(file);
        m_engine->play();
    });
    connect(tools, &StudioTools::placeSoundRequested, this, [this, tv](const QByteArray& sound) {
        placeSound(sound, tv->playhead());
    });
    // After the drop (the drag source waits until dropEvent returns)
    connect(tracks, &StudioTracks::soundDropRequested, this, [this](const QByteArray& sound, int frame) {
        QTimer::singleShot(0, this, [this, sound, frame] { placeSound(sound, frame); });
    });
}
