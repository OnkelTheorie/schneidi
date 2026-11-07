// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the Comp page (0.19) - built lazily, opened for a clip, the viewer's picture of a node or matte, the
// pipette of a key node.
#include "app/MainWindow.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/picture/CompNodes.h"
#include "ui/ColorPanel.h"
#include "ui/KeyColorPicker.h"
#include "ui/KeyPickWindow.h"
#include "ui/Viewer.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompOverlay.h"
#include "ui/comp/CompPage.h"
#include "ui/comp/CompShared.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <algorithm>

void MainWindow::ensureCompPage()
{
    if (m_compPage) return;
    m_compPage = new CompPage(m_editor);
    m_pages->addWidget(m_compPage);
    watchSizes(m_compPage->main(), "compPage");
    watchSizes(m_compPage->top(), "compTop");
    m_compPage->setPlayhead(m_timeline->view()->playhead());
    connect(m_engine, &Engine::positionChanged, m_compPage, [this](int f) {
        if (m_engine->timelinePositions()) m_compPage->setPlayhead(f);
    });
    connect(m_compPage, &CompPage::viewChanged, this, &MainWindow::updatePreview);
    connect(m_compPage, &CompPage::message, this, [this](const QString& t) { statusBar()->showMessage(t, 5000); });
    connect(m_compPage, &CompPage::keyPickRequested, this, &MainWindow::startCompKeyPick);
    // Another video clip selected while the page shows: its graph
    connect(m_selection, &Selection::changed, m_compPage, [this] {
        if (m_page != Page::Comp) return;
        if (const int id = selectedVideoClip()) m_compPage->setClip(id);
    });
}

int MainWindow::selectedVideoClip() const
{
    const Timeline& tl = m_project->timeline();
    const Clip* best = nullptr;
    for (int id : m_editor->clipIdsOf(m_selection->ids())) {
        TrackRef ref;
        const Clip* c = TimelineOps::findClip(tl, id, &ref);
        if (c && ref.kind == TrackKind::Video && (!best || c->start < best->start)) best = c;
    }
    return best ? best->id : 0;
}

void MainWindow::openComp(int clipId)
{
    if (m_page != Page::Comp) showPage(Page::Comp); // may open the selected clip first
    m_compPage->setClip(clipId);
}

void MainWindow::openCompForSelection()
{
    const int id = selectedVideoClip();
    if (!id) {
        statusBar()->showMessage(T("Wähle zuerst einen Videoclip."), 4000);
        return;
    }
    const Clip* c = TimelineOps::findClip(m_project->timeline(), id);
    CompGraph g;
    if (c && !CompUi::graphOf(*c, &g)) { // an empty graph: the clip's picture straight to the output
        g = {};
        g.nodes << CompNode{1, QStringLiteral("clip"), {}};
        g.out = 1;
        QString err;
        if (!m_editor->setCompGraph({id}, g, {}, CompUi::kinds(), &err)) {
            statusBar()->showMessage(T("Geht nicht: %1").arg(err), 5000);
            return;
        }
    }
    openComp(id);
}

void MainWindow::compFromSelection()
{
    if (!m_editor->compFrom()) {
        statusBar()->showMessage(T("Wähle zuerst Videoclips."), 4000);
        return;
    }
    if (const int id = selectedVideoClip()) openComp(id); // compFrom selects the new clip
}

Timeline MainWindow::previewTimeline() const
{
    if (m_page != Page::Comp || !m_compPage || !m_compPage->clip()) return m_project->renderTimeline();
    return CompUi::viewTimeline(m_project->renderTimeline(), m_compPage->clip(), m_compPage->viewNode(),
                                m_compPage->matte());
}

void MainWindow::updatePreview()
{
    m_engine->updateTimeline(previewTimeline());
}

void MainWindow::startCompKeyPick(int clipId, int node, const QString& key)
{
    stopKeyPick();
    m_engine->pause();
    // The picture going into the key node (its src), alone: what the key sees
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    CompGraph g;
    if (!c || !CompUi::graphOf(*c, &g)) return;
    const CompLink* src = g.linkInto(node, QStringLiteral("src"));
    const Timeline view = CompUi::viewTimeline(m_project->renderTimeline(), clipId, src ? src->from : node, false);
    const Timeline alone = KeyColorPicker::isolate(view, clipId);
    const Clip* shown = nullptr;
    for (const Track& t : alone.video)
        if (!t.clips.isEmpty()) shown = &t.clips.first();
    QImage picture;
    if (shown) {
        const int at = m_engine->timelinePositions() ? m_engine->position() : shown->start;
        picture = m_engine->grabFrame(alone, std::clamp(at, shown->start, shown->end() - 1));
    }
    if (picture.isNull()) {
        statusBar()->showMessage(T("Das Bild des Clips konnte nicht erzeugt werden."), 4000);
        return;
    }
    m_keyPicker = new KeyColorPicker(picture, this);
    connect(m_keyPicker, &KeyColorPicker::picked, this, [this, clipId, node, key](const QColor& color) {
        m_editor->setCompNodeValuesAt(clipId, node, {{key, color.name(QColor::HexArgb)}},
                                      m_timeline->view()->playhead());
        m_project->closeMerge();
        stopKeyPick();
    });
    connect(m_keyPicker, &KeyColorPicker::cancelled, this, &MainWindow::stopKeyPick);
    m_keyPickWindow = new KeyPickWindow(m_keyPicker, picture.size(), this);
    connect(m_keyPickWindow, &KeyPickWindow::closeRequested, this, &MainWindow::stopKeyPick);
    m_keyPickWindow->showOn(screen());
}
