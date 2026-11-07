// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompLayers.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "ui/ScrubField.h"
#include "ui/comp/CompShared.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

namespace PC = Picture::Comp;

namespace {

// A plain chain from `start` back to its source: every node reads at most one other node, none twice
bool plainChain(const CompGraph& g, int start, QSet<int>* seen)
{
    for (int id = start; id;) {
        if (seen->contains(id)) return false;
        seen->insert(id);
        const CompLink* in = nullptr;
        for (const CompLink& l : g.links)
            if (l.to == id) {
                if (in) return false;
                in = &l;
            }
        id = in ? in->from : 0;
    }
    return true;
}

} // namespace

bool CompLayers::layersOf(const CompGraph& g, QVector<Layer>* out)
{
    out->clear();
    if (g.isEmpty() || !g.node(g.out)) return false;
    QSet<int> seen;
    int id = g.out;
    while (id) {
        const CompNode* n = g.node(id);
        if (!n || seen.contains(id)) return false;
        if (n->type != QLatin1String("merge")) { // the base picture
            if (!plainChain(g, id, &seen)) return false;
            out->append(Layer{0, id, 0});
            return true;
        }
        seen.insert(id);
        Layer l;
        l.merge = id;
        if (const CompLink* fg = g.linkInto(id, QStringLiteral("fg"))) {
            l.node = fg->from;
            if (!plainChain(g, fg->from, &seen)) return false;
        }
        if (const CompLink* mask = g.linkInto(id, QStringLiteral("mask"))) {
            l.mask = mask->from;
            if (!plainChain(g, mask->from, &seen)) return false;
        }
        out->append(l);
        const CompLink* bg = g.linkInto(id, QStringLiteral("bg"));
        id = bg ? bg->from : 0;
    }
    return true; // the lowest merge has nothing below it
}

CompLayers::CompLayers(Editor* editor, QWidget* parent) : QWidget(parent), m_editor(editor)
{
    setObjectName("comp_layers");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 6, 8, 6);
    lay->setSpacing(4);
    m_listBox = new QWidget;
    m_list = new QVBoxLayout(m_listBox);
    m_list->setContentsMargins(0, 0, 0, 0);
    m_list->setSpacing(3);
    lay->addWidget(m_listBox);
    m_hint = new QWidget;
    auto* hintLay = new QHBoxLayout(m_hint);
    hintLay->setContentsMargins(0, 0, 0, 0);
    m_hintText = new QLabel;
    m_hintText->setObjectName("comp_layers_hint");
    m_hintText->setWordWrap(true);
    m_hintText->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    auto* toNodes = new QPushButton(T("Knoten-Ansicht"));
    toNodes->setObjectName("comp_layers_to_nodes");
    connect(toNodes, &QPushButton::clicked, this, &CompLayers::nodeViewRequested);
    hintLay->addWidget(m_hintText, 1);
    hintLay->addWidget(toNodes);
    lay->addWidget(m_hint);
    lay->addStretch(1);
    refresh();
}

void CompLayers::setClip(int clipId)
{
    m_clipId = clipId;
    refresh();
}

void CompLayers::setSelected(int node)
{
    if (node == m_selected) return;
    m_selected = node;
    for (QAbstractButton* b : m_listBox->findChildren<QAbstractButton*>())
        if (b->property("node").isValid() && b->isCheckable()) b->setChecked(b->property("node").toInt() == node && node);
}

void CompLayers::refresh()
{
    const Clip* c = m_clipId ? TimelineOps::findClip(m_editor->project()->timeline(), m_clipId) : nullptr;
    CompGraph g;
    QVector<Layer> layers;
    const bool has = c && CompUi::graphOf(*c, &g);
    const bool pure = has && layersOf(g, &layers);
    m_hint->setVisible(!pure);
    m_listBox->setVisible(pure);
    if (!pure) {
        m_hintText->setText(!m_clipId ? T("Kein Clip ausgewählt.")
                            : !has    ? T("Dieser Clip hat noch keinen Comp-Graphen.")
                                      : T("Dieser Graph ist keine reine Ebenen-Folge – er lässt sich nur in der "
                                          "Knoten-Ansicht ganz zeigen."));
        m_layers.clear();
        m_shown = {};
        return;
    }
    if (layers != m_layers || g.nodes != m_shown.nodes) rebuild(*c, g);
    m_layers = layers;
    m_shown = g;
    // Values (mode, opacity) of the rows
    for (QComboBox* combo : m_listBox->findChildren<QComboBox*>()) {
        const int merge = combo->property("merge").toInt();
        if (const PC::NodeType* k = PC::find(QStringLiteral("merge"))) {
            const QSignalBlocker block(combo);
            combo->setCurrentIndex(PC::choiceValue(*k, CompUi::nodeValues(*c, merge), QStringLiteral("mode")));
        }
    }
    for (QLineEdit* e : m_listBox->findChildren<QLineEdit*>()) { // ScrubField has no Q_OBJECT
        auto* f = dynamic_cast<ScrubField*>(e);
        if (!f) continue;
        const int merge = f->property("merge").toInt();
        if (const PC::NodeType* k = PC::find(QStringLiteral("merge")))
            f->setValue(CompUi::number(*c, merge, *k, QStringLiteral("opacity"), CompUi::clipFrame(*c, m_playhead)) * 100);
    }
}

void CompLayers::rebuild(const Clip& c, const CompGraph& g)
{
    Q_UNUSED(c);
    while (QLayoutItem* it = m_list->takeAt(0)) {
        if (QWidget* w = it->widget()) { // gone from findChild right away (tests, the value pass below)
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete it;
    }
    QVector<Layer> layers;
    layersOf(g, &layers);
    const PC::NodeType* merge = PC::find(QStringLiteral("merge"));
    const EffectParam* mode = merge ? merge->param(QStringLiteral("mode")) : nullptr;
    // Column titles
    auto* head = new QWidget;
    auto* headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(6, 0, 6, 0);
    const auto title = [&](const QString& t, int stretch, int width = 0) {
        auto* l = new QLabel(t);
        l->setStyleSheet(QString("color: %1; font-size: 8pt;").arg(Theme::textDim.name()));
        if (width) l->setFixedWidth(width);
        headLay->addWidget(l, stretch);
    };
    title(T("Ebene"), 1);
    title(T("Mischmodus"), 0, 150);
    title(T("Maske"), 0, 110);
    title(T("Deckkraft"), 0, 70);
    m_list->addWidget(head);
    for (int i = 0; i < layers.size(); ++i) {
        const Layer& l = layers[i];
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("comp_layer_%1").arg(i));
        row->setAttribute(Qt::WA_StyledBackground);
        row->setStyleSheet(QString("QWidget#%1 { background: %2; border-radius: 3px; }")
                               .arg(row->objectName(), Theme::panelHeader.name()));
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(6, 3, 6, 3);
        auto* name = new QPushButton;
        name->setFlat(true);
        name->setCheckable(true);
        name->setProperty("node", l.node ? l.node : l.merge);
        const CompNode* n = g.node(l.node);
        name->setText(n ? CompUi::nodeName(*n) : T("(leer)"));
        name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        name->setStyleSheet(QString("QPushButton { text-align: left; border: none; background: transparent; color: %1;"
                                    " padding: 2px 4px; }"
                                    "QPushButton:checked { color: %2; font-weight: 600; }")
                                .arg(Theme::text.name(), Theme::primary.name()));
        name->setChecked((l.node ? l.node : l.merge) == m_selected && m_selected);
        const int pick = l.node ? l.node : l.merge;
        connect(name, &QPushButton::clicked, this, [this, pick] {
            setSelected(pick);
            emit selectedChanged(pick);
        });
        lay->addWidget(name, 1);
        if (!l.merge) { // the base picture
            auto* base = new QLabel(T("Basis"));
            base->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
            base->setFixedWidth(150 + 110 + 70 + 12);
            lay->addWidget(base);
            m_list->addWidget(row);
            continue;
        }
        auto* combo = new QComboBox;
        combo->setObjectName(QStringLiteral("comp_layer_mode_%1").arg(i));
        combo->setProperty("merge", l.merge);
        combo->setFixedWidth(150);
        if (mode)
            for (int m = 0; m < mode->choices.size(); ++m)
                combo->addItem(m < mode->choiceLabels.size() ? mode->choiceLabels[m] : mode->choices[m], mode->choices[m]);
        const int mergeId = l.merge;
        connect(combo, &QComboBox::activated, this, [this, combo, mergeId](int index) {
            m_editor->setCompNodeValuesAt(m_clipId, mergeId, {{QStringLiteral("mode"), combo->itemData(index)}}, m_playhead);
            m_editor->project()->closeMerge();
        });
        lay->addWidget(combo);
        const CompNode* mask = g.node(l.mask);
        auto* maskLabel = new QToolButton;
        maskLabel->setText(mask ? CompUi::nodeName(*mask) : QStringLiteral("–"));
        maskLabel->setFixedWidth(110);
        maskLabel->setAutoRaise(true);
        maskLabel->setEnabled(mask != nullptr);
        if (mask) {
            maskLabel->setProperty("node", l.mask);
            maskLabel->setCheckable(true);
            const int maskId = l.mask;
            connect(maskLabel, &QToolButton::clicked, this, [this, maskId] {
                setSelected(maskId);
                emit selectedChanged(maskId);
            });
        }
        lay->addWidget(maskLabel);
        auto* opacity = new ScrubField(0, 100, 0.5, 0);
        opacity->setObjectName(QStringLiteral("comp_layer_opacity_%1").arg(i));
        opacity->setProperty("merge", l.merge);
        opacity->setSuffix(QStringLiteral(" %"));
        opacity->setFixedWidth(70);
        opacity->onChange = [this, mergeId](double v) {
            m_editor->setCompNodeValuesAt(m_clipId, mergeId, {{QStringLiteral("opacity"), v / 100}}, m_playhead,
                                          QStringLiteral("comp:layer:%1:%2").arg(m_clipId).arg(mergeId));
        };
        opacity->onFinish = [this] { m_editor->project()->closeMerge(); };
        lay->addWidget(opacity);
        m_list->addWidget(row);
    }
}
