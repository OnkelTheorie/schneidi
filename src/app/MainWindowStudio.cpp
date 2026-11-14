// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the Studio page (0.23) - built lazily on its first showing, follows the playhead, its clicks seek like the
// timeline's.
#include "app/MainWindow.h"

#include "engine/Engine.h"
#include "ui/studio/StudioPage.h"
#include "ui/studio/StudioTracks.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QSplitter>
#include <QStackedWidget>

void MainWindow::ensureStudioPage()
{
    if (m_studioPage) return;
    m_studioPage = new StudioPage(m_editor, m_engine, m_mediaCache);
    m_pages->addWidget(m_studioPage);
    watchSizes(m_studioPage->root(), "studioRoot");
    watchSizes(m_studioPage->top(), "studioTop");
    TimelineView* tv = m_timeline->view();
    StudioTracks* tracks = m_studioPage->tracks();
    // The lanes start where the timeline is (its zoom and scroll, once); from then on they are the user's own
    tracks->setView(tv->view().pxPerFrame, tv->view().leftFrame);
    tracks->setPlayhead(tv->playhead());
    connect(m_engine, &Engine::positionChanged, tracks, [this, tracks](int f) {
        if (m_engine->timelinePositions()) tracks->setPlayhead(f);
    });
    connect(tracks, &StudioTracks::seekRequested, tv, &TimelineView::seekRequested);
}
