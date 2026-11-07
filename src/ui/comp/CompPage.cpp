// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompPage.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "ui/comp/CompCanvas.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompLayers.h"
#include "ui/comp/CompOverlay.h"
#include "ui/comp/CompShared.h"

#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

CompPage::CompPage(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("comp_page");
    m_inspector = new CompInspector(editor);
    m_canvas = new CompCanvas(editor);
    m_layers = new CompLayers(editor);
    m_overlay = new CompOverlay(editor, this);

    // Bar above the graph: clip, layers/nodes, what the viewer shows
    auto* bar = new QWidget;
    bar->setObjectName("CompBar");
    bar->setAttribute(Qt::WA_StyledBackground);
    bar->setStyleSheet(QString("QWidget#CompBar { background: %1; border-bottom: 1px solid %2; }")
                           .arg(Theme::panelHeader.name(), Theme::border.name()));
    auto* barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(8, 3, 8, 3);
    barLay->setSpacing(4);
    const auto toggle = [&](const QString& name, const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    m_layersButton = toggle("comp_view_layers", T("Ebenen"), T("Der Graph als Ebenen von oben nach unten"));
    m_nodesButton = toggle("comp_view_nodes", T("Knoten"), T("Der Graph als Knoten mit Verbindungen"));
    auto* group = new QButtonGroup(this);
    group->addButton(m_layersButton);
    group->addButton(m_nodesButton);
    m_layersButton->setChecked(true);
    m_clipLabel = new QLabel;
    m_clipLabel->setObjectName("comp_clip_label");
    m_clipLabel->setStyleSheet("font-weight: 600;");
    m_message = new QLabel;
    m_message->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
    m_create = new QPushButton(T("Comp anlegen"));
    m_create->setObjectName("comp_create");
    m_create->setToolTip(T("Gibt dem Clip einen Graphen: sein Bild geht unverändert zum Ausgang"));
    m_showNode = toggle("comp_view_node", T("Knoten zeigen"), T("Der Viewer zeigt das Bild des gewählten Knotens statt "
                                                                "des Ausgangs (Doppelklick auf einen Knoten)"));
    m_matte = toggle("comp_view_matte", T("Matte"), T("Der Viewer zeigt die Deckkraft (Alpha) als Grau"));
    auto* fit = new QToolButton;
    fit->setText(T("Einpassen"));
    fit->setToolTip(T("Den ganzen Graphen zeigen (F)"));
    fit->setFocusPolicy(Qt::NoFocus);
    connect(fit, &QToolButton::clicked, m_canvas, &CompCanvas::fit);
    barLay->addWidget(m_clipLabel);
    barLay->addSpacing(10);
    barLay->addWidget(m_layersButton);
    barLay->addWidget(m_nodesButton);
    barLay->addWidget(fit);
    barLay->addSpacing(10);
    barLay->addWidget(m_create);
    barLay->addWidget(m_message, 1);
    barLay->addWidget(m_showNode);
    barLay->addWidget(m_matte);

    m_stack = new QStackedWidget;
    auto* layersScroll = new QScrollArea;
    layersScroll->setWidgetResizable(true);
    layersScroll->setFrameShape(QFrame::NoFrame);
    layersScroll->setWidget(m_layers);
    m_stack->addWidget(layersScroll);
    m_stack->addWidget(m_canvas);
    auto* bottom = new QWidget;
    auto* bottomLay = new QVBoxLayout(bottom);
    bottomLay->setContentsMargins(0, 0, 0, 0);
    bottomLay->setSpacing(0);
    bottomLay->addWidget(bar);
    bottomLay->addWidget(m_stack, 1);

    m_top = new QSplitter(Qt::Horizontal);
    m_top->setObjectName("compTop");
    m_top->addWidget(m_inspector);
    m_main = new QSplitter(Qt::Vertical);
    m_main->setObjectName("compPage");
    m_main->addWidget(m_top);
    m_main->addWidget(bottom);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_main);

    connect(m_layersButton, &QToolButton::toggled, this, [this](bool on) {
        m_stack->setCurrentIndex(on ? 0 : 1);
        if (!on) QTimer::singleShot(0, m_canvas, [this] { m_canvas->refresh(); }); // laid out now: a first fit
    });
    connect(m_layers, &CompLayers::nodeViewRequested, this, [this] { setView(View::Nodes); });
    connect(m_canvas, &CompCanvas::selectedChanged, this, &CompPage::selectNode);
    connect(m_layers, &CompLayers::selectedChanged, this, &CompPage::selectNode);
    connect(m_canvas, &CompCanvas::showRequested, this, [this](int node) {
        selectNode(node);
        setShowNode(true);
    });
    connect(m_canvas, &CompCanvas::message, this, [this](const QString& t) {
        m_message->setText(t);
        QTimer::singleShot(5000, m_message, [this, t] {
            if (m_message->text() == t) m_message->clear();
        });
        emit message(t);
    });
    connect(m_inspector, &CompInspector::outRequested, this, [this](int node) {
        m_editor->setCompOut(m_clipId, node, CompUi::kinds());
    });
    connect(m_inspector, &CompInspector::keyPickRequested, this,
            [this](int node, const QString& key) { emit keyPickRequested(m_clipId, node, key); });
    connect(m_showNode, &QToolButton::toggled, this, &CompPage::viewChanged);
    connect(m_matte, &QToolButton::toggled, this, &CompPage::viewChanged);
    connect(m_create, &QPushButton::clicked, this, [this] {
        CompGraph g;
        g.nodes << CompNode{1, QStringLiteral("clip"), {}};
        g.out = 1;
        m_editor->setCompGraph({m_clipId}, g, {}, CompUi::kinds());
    });
    connect(m_editor->project(), &Project::timelineChanged, this, &CompPage::refresh);
    refresh();
}

void CompPage::setClip(int clipId)
{
    if (clipId == m_clipId) {
        refresh();
        return;
    }
    m_clipId = clipId;
    m_selected = 0;
    m_canvas->setClip(clipId);
    m_layers->setClip(clipId);
    m_inspector->setTarget(clipId, 0);
    m_overlay->setTarget(clipId, 0, m_playhead);
    refresh();
    emit viewChanged();
}

void CompPage::setPlayhead(int frame)
{
    m_playhead = frame;
    m_inspector->setPlayhead(frame);
    m_layers->setPlayhead(frame);
    m_overlay->setTarget(m_clipId, m_selected, frame);
}

void CompPage::selectNode(int node)
{
    if (node == m_selected) return;
    m_selected = node;
    m_canvas->setSelected(node);
    m_layers->setSelected(node);
    m_inspector->setTarget(m_clipId, node);
    m_overlay->setTarget(m_clipId, node, m_playhead);
    if (showNode()) emit viewChanged();
}

void CompPage::refresh()
{
    const Clip* c = m_clipId ? TimelineOps::findClip(m_editor->project()->timeline(), m_clipId) : nullptr;
    CompGraph g;
    const bool has = c && CompUi::graphOf(*c, &g);
    if (m_selected && !g.node(m_selected)) { // removed (an undo, the CLI): no node selected
        m_selected = 0;
        m_canvas->setSelected(0);
        m_layers->setSelected(0);
        m_inspector->setTarget(m_clipId, 0);
        m_overlay->setTarget(m_clipId, 0, m_playhead);
        if (showNode()) emit viewChanged();
    }
    m_canvas->refresh();
    m_layers->refresh();
    m_inspector->refresh();
    m_overlay->refresh();
    m_create->setVisible(c && !has);
    updateHeader();
}

void CompPage::updateHeader()
{
    const Clip* c = m_clipId ? TimelineOps::findClip(m_editor->project()->timeline(), m_clipId) : nullptr;
    CompGraph g;
    const bool has = c && CompUi::graphOf(*c, &g);
    m_clipLabel->setText(!c ? T("Kein Clip") : has ? T("%1 – %2 Knoten").arg(m_editor->project()->clipName(*c)).arg(g.nodes.size())
                                                   : m_editor->project()->clipName(*c));
}

void CompPage::setView(View v)
{
    (v == View::Layers ? m_layersButton : m_nodesButton)->setChecked(true);
}

CompPage::View CompPage::view() const { return m_layersButton->isChecked() ? View::Layers : View::Nodes; }
void CompPage::setShowNode(bool on) { m_showNode->setChecked(on); }
bool CompPage::showNode() const { return m_showNode->isChecked(); }
void CompPage::setMatte(bool on) { m_matte->setChecked(on); }
bool CompPage::matte() const { return m_matte->isChecked(); }
int CompPage::viewNode() const { return showNode() ? m_selected : 0; }
