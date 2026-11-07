// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompInspector.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"
#include "ui/ScrubField.h"
#include "ui/comp/CompShared.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

namespace PC = Picture::Comp;

namespace {

void paintSwatch(QToolButton* b, const QColor& c)
{
    b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                         .arg(c.name(QColor::HexArgb), Theme::border.name()));
    b->setToolTip(c.name(QColor::HexArgb));
}

// Mask points: p<i>x/p<i>y (shown as many as the shape uses); bezier handles h<i>… only in the viewer
bool pointKey(const QString& key, int* index)
{
    static const QRegularExpression re(QStringLiteral("^p(\\d+)[xy]$"));
    const auto m = re.match(key);
    if (!m.hasMatch()) return false;
    *index = m.captured(1).toInt();
    return true;
}

bool handleKey(const QString& key)
{
    static const QRegularExpression re(QStringLiteral("^h\\d+i?[xy]$"));
    return re.match(key).hasMatch();
}

} // namespace

CompInspector::CompInspector(Editor* editor, QWidget* parent) : QScrollArea(parent), m_editor(editor)
{
    setObjectName("comp_inspector");
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    m_body = new QWidget;
    auto* lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);
    auto* head = new QHBoxLayout;
    m_title = new QLabel;
    m_title->setObjectName("comp_inspector_title");
    m_title->setStyleSheet("font-weight: 600;");
    m_outButton = new QToolButton;
    m_outButton->setObjectName("comp_set_out");
    m_outButton->setText(T("Als Ausgang"));
    m_outButton->setToolTip(T("Das Bild dieses Knotens wird das Bild des Clips"));
    connect(m_outButton, &QToolButton::clicked, this, [this] { emit outRequested(m_node); });
    head->addWidget(m_title, 1);
    head->addWidget(m_outButton);
    lay->addLayout(head);
    m_about = new QLabel;
    m_about->setWordWrap(true);
    m_about->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    lay->addWidget(m_about);
    m_rows = new QWidget;
    m_grid = new QGridLayout(m_rows);
    m_grid->setContentsMargins(0, 4, 0, 0);
    m_grid->setHorizontalSpacing(6);
    m_grid->setVerticalSpacing(4);
    m_grid->setColumnStretch(1, 1);
    lay->addWidget(m_rows);
    lay->addStretch(1);
    setWidget(m_body);
    setMinimumWidth(260);
    refresh();
}

void CompInspector::setTarget(int clipId, int node)
{
    if (clipId == m_clipId && node == m_node) return;
    m_clipId = clipId;
    m_node = node;
    refresh();
}

void CompInspector::setPlayhead(int frame)
{
    if (frame == m_playhead) return;
    m_playhead = frame;
    if (const Clip* c = clip())
        for (const auto& u : m_updates) u(*c);
}

const Clip* CompInspector::clip() const
{
    return m_clipId ? TimelineOps::findClip(m_editor->project()->timeline(), m_clipId) : nullptr;
}

QStringList CompInspector::shownKeys(const Clip& c) const
{
    QStringList keys;
    CompGraph g;
    if (!CompUi::graphOf(c, &g)) return keys;
    const CompNode* n = g.node(m_node);
    const PC::NodeType* k = n ? PC::find(n->type) : nullptr;
    if (!k) return keys;
    const QVariantMap values = CompUi::nodeValues(c, m_node);
    int points = 0;
    if (k->id == QLatin1String("mask")) {
        const QString shape = PC::textValue(*k, values, QStringLiteral("shape"));
        points = shape == QLatin1String("polygon") || shape == QLatin1String("bezier")
                     ? int(PC::numValue(*k, values, QStringLiteral("points")))
                 : shape == QLatin1String("linear") || shape == QLatin1String("radial") ? 2
                                                                                         : 0;
    }
    for (const EffectParam& p : k->params) {
        int index = 0;
        if (k->id == QLatin1String("mask") && pointKey(p.key, &index) && index >= points) continue;
        if (k->id == QLatin1String("mask") && handleKey(p.key)) continue;
        keys << p.key;
    }
    return keys;
}

void CompInspector::refresh()
{
    const Clip* c = clip();
    CompGraph g;
    const CompNode* n = c && CompUi::graphOf(*c, &g) ? g.node(m_node) : nullptr;
    if (!n) {
        m_title->setText(T("Kein Knoten ausgewählt"));
        m_about->setText(T("Wähle einen Knoten in der Knoten- oder Ebenen-Ansicht."));
        m_outButton->hide();
        if (!m_keys.isEmpty() || !m_type.isEmpty()) rebuild(Clip{}, {});
        m_type.clear();
        return;
    }
    const PC::NodeType* k = PC::find(n->type);
    m_title->setText(CompUi::nodeName(*n));
    m_about->setText(k ? k->description : T("Diese Knotenart kennt diese Version nicht; sie reicht ihr Bild durch."));
    m_outButton->setVisible(n->id != g.out);
    const QStringList keys = shownKeys(*c);
    if (keys != m_keys || n->type != m_type) {
        m_type = n->type;
        rebuild(*c, keys);
    }
    for (const auto& u : m_updates) u(*c);
}

void CompInspector::set(const QString& key, const QVariant& value, bool drag)
{
    QString err;
    const QString merge = drag ? QStringLiteral("comp:%1:%2:%3").arg(m_clipId).arg(m_node).arg(key) : QString();
    m_editor->setCompNodeValuesAt(m_clipId, m_node, {{key, value}}, m_playhead, merge, &err);
    if (!drag) m_editor->project()->closeMerge();
}

void CompInspector::rebuild(const Clip& c, const QStringList& keys)
{
    m_updates.clear();
    m_keys = keys;
    while (QLayoutItem* it = m_grid->takeAt(0)) {
        if (QWidget* w = it->widget()) { // gone from findChild right away (tests, the value pass below)
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete it;
    }
    CompGraph g;
    const CompNode* n = CompUi::graphOf(c, &g) ? g.node(m_node) : nullptr;
    const PC::NodeType* k = n ? PC::find(n->type) : nullptr;
    if (!k) return;
    const QStringList files = Comp::fileKeys(k->id);
    int row = 0;
    for (const QString& key : keys) {
        const EffectParam* p = k->param(key);
        if (!p) continue;
        auto* label = new QLabel(p->label);
        label->setToolTip(p->description);
        m_grid->addWidget(label, row, 0);
        const int node = m_node;
        const QString kind = k->id;
        switch (p->type) {
        case EffectParam::Double: {
            auto* field = new ScrubField(p->min, p->max, p->step > 0 ? p->step : 0.01, p->decimals);
            field->setObjectName("comp_param_" + key);
            field->setToolTip(p->description);
            field->onChange = [this, key](double v) { set(key, v, true); };
            field->onFinish = [this] { m_editor->project()->closeMerge(); };
            auto* diamond = new QToolButton;
            diamond->setObjectName("comp_key_" + key);
            diamond->setAutoRaise(true);
            diamond->setFocusPolicy(Qt::NoFocus);
            diamond->setToolTip(T("Keyframe am Playhead setzen/entfernen"));
            connect(diamond, &QToolButton::clicked, this, [this, key, field] {
                const Clip* cl = clip();
                if (!cl) return;
                const bool on = !CompUi::keyAt(*cl, m_node, key, CompUi::clipFrame(*cl, m_playhead));
                m_editor->setCompNodeKeyframe(m_clipId, m_node, key, m_playhead, on, field->value());
            });
            m_grid->addWidget(field, row, 1);
            m_grid->addWidget(diamond, row, 2);
            m_updates << [this, field, diamond, node, key, kind](const Clip& cl) {
                const PC::NodeType* kt = PC::find(kind);
                if (!kt) return;
                const int t = CompUi::clipFrame(cl, m_playhead);
                field->setValue(CompUi::number(cl, node, *kt, key, t));
                const bool at = CompUi::keyAt(cl, node, key, t), any = CompUi::animated(cl, node, key);
                diamond->setText(at ? QStringLiteral("◆") : QStringLiteral("◇"));
                diamond->setStyleSheet(QString("QToolButton { color: %1; border: none; }")
                                           .arg((at ? Theme::warning : any ? Theme::secondary : Theme::textDim).name()));
            };
            break;
        }
        case EffectParam::Bool: {
            auto* box = new QCheckBox;
            box->setObjectName("comp_param_" + key);
            connect(box, &QCheckBox::toggled, this, [this, key](bool on) { set(key, on, false); });
            m_grid->addWidget(box, row, 1, 1, 2);
            m_updates << [box, node, key, kind](const Clip& cl) {
                const PC::NodeType* kt = PC::find(kind);
                if (!kt) return;
                const QSignalBlocker block(box);
                box->setChecked(PC::numValue(*kt, CompUi::nodeValues(cl, node), key) != 0);
            };
            break;
        }
        case EffectParam::Choice: {
            auto* combo = new QComboBox;
            combo->setObjectName("comp_param_" + key);
            for (int i = 0; i < p->choices.size(); ++i)
                combo->addItem(i < p->choiceLabels.size() ? p->choiceLabels[i] : p->choices[i], p->choices[i]);
            connect(combo, &QComboBox::activated, this,
                    [this, key, combo](int i) { set(key, combo->itemData(i).toString(), false); });
            m_grid->addWidget(combo, row, 1, 1, 2);
            m_updates << [combo, node, key, kind](const Clip& cl) {
                const PC::NodeType* kt = PC::find(kind);
                if (!kt) return;
                const QSignalBlocker block(combo);
                combo->setCurrentIndex(PC::choiceValue(*kt, CompUi::nodeValues(cl, node), key));
            };
            break;
        }
        case EffectParam::Color: {
            auto* swatch = new QToolButton;
            swatch->setObjectName("comp_param_" + key);
            swatch->setFixedHeight(20);
            swatch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            connect(swatch, &QToolButton::clicked, this, [this, key, node, kind] {
                const Clip* cl = clip();
                const PC::NodeType* kt = PC::find(kind);
                if (!cl || !kt) return;
                const QColor now = PC::colorValue(*kt, CompUi::nodeValues(*cl, node), key);
                const QColor c = QColorDialog::getColor(now, this, T("Farbe wählen"), QColorDialog::ShowAlphaChannel);
                if (c.isValid()) set(key, c.name(QColor::HexArgb), false);
            });
            m_grid->addWidget(swatch, row, 1);
            if (kind == QLatin1String("key")) { // the key color from the picture
                auto* pipette = new QToolButton;
                pipette->setObjectName("comp_pipette_" + key);
                pipette->setText(QStringLiteral("⊙"));
                pipette->setAutoRaise(true);
                pipette->setToolTip(T("Farbe im Bild aufnehmen"));
                connect(pipette, &QToolButton::clicked, this, [this, key] { emit keyPickRequested(m_node, key); });
                m_grid->addWidget(pipette, row, 2);
            }
            m_updates << [swatch, node, key, kind](const Clip& cl) {
                const PC::NodeType* kt = PC::find(kind);
                if (kt) paintSwatch(swatch, PC::colorValue(*kt, CompUi::nodeValues(cl, node), key));
            };
            break;
        }
        case EffectParam::Path: {
            auto* edit = new QLineEdit;
            edit->setObjectName("comp_param_" + key);
            connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] {
                const Clip* cl = clip();
                if (cl && CompUi::nodeValues(*cl, m_node).value(key).toString() != edit->text()) set(key, edit->text(), false);
            });
            m_grid->addWidget(edit, row, 1);
            if (files.contains(key)) {
                auto* browse = new QToolButton;
                browse->setText(QStringLiteral("…"));
                browse->setToolTip(T("Datei wählen"));
                connect(browse, &QToolButton::clicked, this, [this, key, edit] {
                    const QString f = QFileDialog::getOpenFileName(this, T("Datei wählen"),
                                                                   QFileInfo(edit->text()).absolutePath());
                    if (!f.isEmpty()) set(key, f, false);
                });
                m_grid->addWidget(browse, row, 2);
            }
            m_updates << [edit, node, key, kind](const Clip& cl) {
                const PC::NodeType* kt = PC::find(kind);
                if (!kt || edit->hasFocus()) return;
                const QSignalBlocker block(edit);
                edit->setText(PC::textValue(*kt, CompUi::nodeValues(cl, node), key));
            };
            break;
        }
        }
        ++row;
    }
}
