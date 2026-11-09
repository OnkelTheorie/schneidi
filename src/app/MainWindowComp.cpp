// SPDX-License-Identifier: AGPL-3.0-only
// MainWindow: the Comp page (0.19) - built lazily, opened for a clip, the viewer's picture of a node or matte, the
// pipette of a key node.
#include "app/MainWindow.h"
#include "ui/TourOverlay.h"
#include "app/TourManager.h"

#include "app/ExtensionsDialog.h"
#include "core/CompRecipes.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/picture/CompNodes.h"
#include "ui/ColorPanel.h"
#include "ui/EffectsLibrary.h"
#include "ui/KeyColorPicker.h"
#include "ui/KeyPickWindow.h"
#include "ui/Viewer.h"
#include "ui/comp/CompCanvas.h"
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
    watchSizes(m_compPage->main(), "compRoot");
    watchSizes(m_compPage->top(), "compLeft");
    m_compPage->setPlayhead(m_timeline->view()->playhead());
    connect(m_engine, &Engine::positionChanged, m_compPage, [this](int f) {
        if (!m_engine->timelinePositions()) return;
        // The page follows the playhead: the clip under it (in a gap the last one stays)
        if (m_page == Page::Comp)
            if (const int id = compClipAt(f); id && id != m_compPage->clip()) m_compPage->setClip(id);
        m_compPage->setPlayhead(f);
    });
    connect(m_compPage, &CompPage::viewChanged, this, &MainWindow::updatePreview);
    m_compPage->setTourRunning(m_tourSaved.active);
    connect(m_compPage->canvas(), &CompCanvas::tourRequested, this, [this] { // the button in an empty graph
        if (m_tours) m_tours->start("compositing");
    });
    connect(m_compPage, &CompPage::message, this, [this](const QString& t) { statusBar()->showMessage(t, 5000); });
    connect(m_compPage, &CompPage::keyPickRequested, this, &MainWindow::startCompKeyPick);
    connect(m_compPage, &CompPage::recipeSaved, m_effects, &EffectsLibrary::refreshRecipes);
    connect(m_compPage, &CompPage::extensionsRequested, this, [this] {
        ExtensionsDialog ext(this);
        ext.exec();
        m_compPage->refresh(); // the model may be there now
    });
    // Another video clip selected while the page shows: its graph when it is under the playhead
    connect(m_selection, &Selection::changed, m_compPage, [this] {
        if (m_page != Page::Comp) return;
        playheadInto(selectedVideoClip()); // the viewer shows what the page works on
        if (const int id = compClipAt(m_timeline->view()->playhead()); id && id != m_compPage->clip())
            m_compPage->setClip(id);
    });
}

bool MainWindow::applyRecipe(const QVector<int>& ids, const QString& recipe)
{
    if (CompUi::applyRecipeAsking(this, m_editor, ids, recipe).isNull()) return false;
    const std::optional<CompRecipe> r = CompRecipes::find(recipe);
    statusBar()->showMessage(T("Rezept „%1“ angewendet – die Werte stellst du auf der Seite Compositing ein")
                                 .arg(r ? CompRecipes::shownName(*r) : recipe), 6000);
    return true;
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

int MainWindow::compClipAt(int frame) const
{
    const Timeline& tl = m_project->timeline();
    const auto under = [frame](const Clip& c) { return c.start <= frame && frame < c.end(); };
    if (const int id = selectedVideoClip())
        if (const Clip* c = TimelineOps::findClip(tl, id); c && under(*c)) return id;
    for (qsizetype t = tl.video.size() - 1; t >= 0; --t) { // the topmost track first
        if (tl.video[t].hidden) continue;
        for (const Clip& c : tl.video[t].clips)
            if (c.enabled && under(c)) return c.id; // a disabled clip is not in the picture
    }
    return 0;
}

void MainWindow::playheadInto(int clipId)
{
    const Clip* c = clipId && !m_tourSwap ? TimelineOps::findClip(m_project->timeline(), clipId) : nullptr;
    if (!c) return; // also while a tour gives the user's project back: their playhead stays
    const int at = m_timeline->view()->playhead();
    if (at >= c->start && at < c->end()) return;
    if (m_engine->mode() == Engine::Mode::Source) m_engine->showTimeline(c->start);
    else m_engine->seek(c->start);
}

void MainWindow::openComp(int clipId)
{
    // The page follows the playhead: it goes into the clip first (else the next frame would show another one)
    playheadInto(clipId);
    if (m_page != Page::Comp) showPage(Page::Comp);
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
    if (m_tours) m_tours->overlay()->setPaused(true); // the bubble would cover the picture
    m_keyPickWindow->showOn(screen());
}
