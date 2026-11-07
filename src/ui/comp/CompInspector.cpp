// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/comp/CompInspector.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/EffectRegistry.h"
#include "engine/picture/CompNodes.h"
#include "engine/PersonMatte.h"
#include "engine/picture/CompCutout.h"
#include "engine/picture/CompTrack.h"
#include "engine/picture/CustomEffects.h"
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
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QToolButton>
#include <QPointer>
#include <QThreadPool>
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

CompInspector::~CompInspector()
{
    cancelAnalysis(); // the thread keeps its own copy of everything; it only stops early
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

std::shared_ptr<const PC::NodeType> CompInspector::shownKind(const Clip& c) const
{
    CompGraph g;
    if (!CompUi::graphOf(c, &g)) return {};
    const CompNode* n = g.node(m_node);
    const PC::NodeType* k = n ? PC::find(n->type) : nullptr;
    if (!k) return {};
    auto out = std::make_shared<PC::NodeType>(*k);
    if (k->id == QLatin1String("fx")) {
        const QString effect = PC::textValue(*k, CompUi::nodeValues(c, m_node), QStringLiteral("effect"));
        if (const EffectDescriptor* d = EffectRegistry::find(effect))
            for (EffectParam p : d->params) {
                p.key = QStringLiteral("e_") + p.key;
                out->params << p;
            }
    }
    return out;
}

QStringList CompInspector::shownKeys(const Clip& c) const
{
    QStringList keys;
    const std::shared_ptr<const PC::NodeType> kp = shownKind(c);
    const PC::NodeType* k = kp.get();
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
    updateAnalysisRow();
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
    m_analyzeButton = nullptr;
    m_analyzeStatus = nullptr;
    m_extensionsButton = nullptr;
    while (QLayoutItem* it = m_grid->takeAt(0)) {
        if (QWidget* w = it->widget()) { // gone from findChild right away (tests, the value pass below)
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete it;
    }
    const std::shared_ptr<const PC::NodeType> kp = shownKind(c);
    const PC::NodeType* k = kp.get();
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
            m_updates << [this, field, diamond, node, key, kp](const Clip& cl) {
                const PC::NodeType* kt = kp.get();
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
            m_updates << [box, node, key, kp](const Clip& cl) {
                const PC::NodeType* kt = kp.get();
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
            m_updates << [combo, node, key, kp](const Clip& cl) {
                const PC::NodeType* kt = kp.get();
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
            connect(swatch, &QToolButton::clicked, this, [this, key, node, kp] {
                const Clip* cl = clip();
                const PC::NodeType* kt = kp.get();
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
            m_updates << [swatch, node, key, kp](const Clip& cl) {
                paintSwatch(swatch, PC::colorValue(*kp, CompUi::nodeValues(cl, node), key));
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
            m_updates << [edit, node, key, kp](const Clip& cl) {
                const PC::NodeType* kt = kp.get();
                if (!kt || edit->hasFocus()) return;
                const QSignalBlocker block(edit);
                edit->setText(PC::textValue(*kt, CompUi::nodeValues(cl, node), key));
            };
            break;
        }
        case EffectParam::Text: { // a filter chain / shader code: typed, then checked and set with "Anwenden"
            label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            auto* edit = new QPlainTextEdit;
            edit->setObjectName("comp_param_" + key);
            edit->setToolTip(p->description);
            edit->setTabChangesFocus(true);
            edit->setMinimumHeight(90);
            QFont mono = edit->font();
            mono.setFamily(QStringLiteral("monospace"));
            mono.setStyleHint(QFont::Monospace);
            edit->setFont(mono);
            auto* applyButton = new QPushButton(T("Anwenden"));
            applyButton->setObjectName("comp_apply_" + key);
            auto* error = new QLabel;
            error->setObjectName("comp_error_" + key);
            error->setWordWrap(true);
            error->setTextInteractionFlags(Qt::TextSelectableByMouse);
            error->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
            error->hide();
            connect(applyButton, &QPushButton::clicked, this, [this, key, edit, error] {
                const QString text = edit->toPlainText();
                // The same check as the CLI's node values (cli/EditOpsComp.cpp)
                QString why;
                if (key == QLatin1String("e_chain")) why = Picture::Custom::checkFilter(text);
                else if (key == QLatin1String("e_code")) why = Picture::Custom::checkShader(text);
                error->setText(why);
                error->setVisible(!why.isEmpty());
                if (why.isEmpty()) set(key, text, false);
            });
            m_grid->addWidget(edit, row, 1, 1, 2);
            ++row;
            auto* buttons = new QHBoxLayout;
            buttons->addStretch(1);
            buttons->addWidget(applyButton);
            auto* buttonRow = new QWidget;
            buttonRow->setLayout(buttons);
            buttons->setContentsMargins(0, 0, 0, 0);
            m_grid->addWidget(buttonRow, row, 1, 1, 2);
            ++row;
            m_grid->addWidget(error, row, 0, 1, 3);
            m_updates << [edit, node, key, kp](const Clip& cl) {
                if (edit->hasFocus()) return;
                const QString now = PC::textValue(*kp, CompUi::nodeValues(cl, node), key);
                if (edit->toPlainText() != now) edit->setPlainText(now);
            };
            break;
        }
        }
        ++row;
    }
    if (k->id == QLatin1String(PC::kCutoutType) || k->id == QLatin1String(::Comp::kTrackerType))
        row = addAnalysisRow(row, k->id);
}

int CompInspector::addAnalysisRow(int row, const QString& kind)
{
    const bool cutout = kind == QLatin1String(PC::kCutoutType);
    m_analyzeButton = new QPushButton(cutout ? T("Freistellen analysieren") : T("Tracken"));
    m_analyzeButton->setObjectName("comp_analyze");
    m_analyzeButton->setToolTip(cutout ? T("Sucht die Person einmal im ganzen Clip (KI-Modell); danach stellt der "
                                           "Knoten sie in jedem Bild frei")
                                       : T("Verfolgt die Startpunkte vom Playhead bis zum Clipende; das Ergebnis "
                                           "sind Keyframes, denen andere Knoten folgen"));
    connect(m_analyzeButton, &QPushButton::clicked, this, [this] {
        if (m_job && m_job->clipId == m_clipId && m_job->node == m_node) cancelAnalysis();
        else startAnalysis();
    });
    m_analyzeStatus = new QLabel;
    m_analyzeStatus->setObjectName("comp_analyze_status");
    m_analyzeStatus->setWordWrap(true);
    m_extensionsButton = new QPushButton(T("Erweiterungen öffnen…"));
    m_extensionsButton->setObjectName("comp_open_extensions");
    connect(m_extensionsButton, &QPushButton::clicked, this, &CompInspector::extensionsRequested);
    m_grid->addWidget(m_analyzeButton, row++, 0, 1, 3);
    m_grid->addWidget(m_analyzeStatus, row++, 0, 1, 3);
    m_grid->addWidget(m_extensionsButton, row++, 0, 1, 3, Qt::AlignLeft);
    updateAnalysisRow();
    return row;
}

void CompInspector::updateAnalysisRow()
{
    if (!m_analyzeButton) return;
    const bool cutout = m_type == QLatin1String(PC::kCutoutType);
    const bool mine = m_job && m_job->clipId == m_clipId && m_job->node == m_node;
    QString why;
    const bool missing = cutout && !PersonMatte::available(&why);
    m_analyzeButton->setText(mine ? T("Abbrechen") : cutout ? T("Freistellen analysieren") : T("Tracken"));
    m_analyzeButton->setEnabled(mine || (!m_job && !missing));
    QString text;
    QColor color = Theme::textDim;
    if (mine) {
        text = m_job->total > 0 ? T("Analysiert … %1 %").arg(100 * m_job->done / m_job->total) : T("Analysiert …");
    } else if (missing) {
        text = PersonMatte::needsExtension();
        color = Theme::warning;
    } else if (m_noteNode == m_node && !m_analysisNote.isEmpty()) {
        text = m_analysisNote;
    } else if (m_job) {
        text = T("Eine andere Analyse läuft noch.");
    } else if (cutout) {
        if (const Clip* c = clip()) {
            const QString cache = CompUi::nodeValues(*c, m_node).value(QString::fromLatin1(PC::kCutoutCacheKey)).toString();
            const QString hint = PC::cutoutHint(cache);
            text = hint.isEmpty() ? T("Analysiert – erneut analysieren ersetzt das Ergebnis") : hint;
        }
    }
    m_analyzeStatus->setText(text);
    m_analyzeStatus->setStyleSheet(QString("color: %1;").arg(color.name()));
    m_analyzeStatus->setVisible(!text.isEmpty());
    m_extensionsButton->setVisible(missing);
}

void CompInspector::startAnalysis()
{
    const Clip* c = clip();
    if (!c || m_job || (m_type != QLatin1String(PC::kCutoutType) && m_type != QLatin1String(::Comp::kTrackerType))) return;
    Job job;
    job.clipId = m_clipId;
    job.node = m_node;
    job.kind = m_type;
    job.cancel = std::make_shared<std::atomic<bool>>(false);
    m_job = job;
    m_analysisNote.clear();
    updateAnalysisRow();
    const Clip copy = *c; // the thread works on its own copy (the timeline may change meanwhile)
    const ProjectFormat format = m_editor->project()->timelineFormat();
    const int last = std::max(0, copy.length() - 1);
    const int from = m_type == QLatin1String(::Comp::kTrackerType) ? CompUi::clipFrame(copy, m_playhead) : 0;
    QPointer<CompInspector> guard(this);
    const auto progress = [guard](int done, int total) {
        QMetaObject::invokeMethod(guard, [guard, done, total] {
            if (!guard || !guard->m_job) return;
            guard->m_job->done = done;
            guard->m_job->total = total;
            guard->updateAnalysisRow();
        }, Qt::QueuedConnection);
    };
    QThreadPool::globalInstance()->start([guard, copy, format, from, last, job, progress] {
        QString cache, error, note;
        QVector<::Comp::TrackPoint> points;
        if (job.kind == QLatin1String(PC::kCutoutType)) {
            const PC::CutoutRun r = PC::cutoutNode(format, copy, job.node, 0, last, progress, job.cancel.get());
            error = r.canceled ? T("Abgebrochen") : r.error;
            if (error.isEmpty() && r.frames <= 0) error = T("Nichts analysiert");
            cache = r.cacheId;
        } else {
            const PC::TrackRun r = PC::trackNode(format, copy, job.node, from, last, progress, job.cancel.get());
            error = r.canceled ? T("Abgebrochen") : r.error;
            if (error.isEmpty() && r.points.isEmpty()) error = T("Nichts verfolgt");
            points = r.points;
            if (r.gaveUp) note = T("Zu lange verloren: die Spur endet dort");
        }
        QMetaObject::invokeMethod(guard, [guard, job, cache, points, error, note] {
            if (guard) guard->analysisDone(job.clipId, job.node, job.kind, cache, points, error, note);
        }, Qt::QueuedConnection);
    });
}

void CompInspector::cancelAnalysis()
{
    if (m_job) m_job->cancel->store(true);
}

void CompInspector::analysisDone(int clipId, int node, const QString& kind, const QString& cache,
                                 const QVector<::Comp::TrackPoint>& points, const QString& error, const QString& note)
{
    m_job.reset();
    m_noteNode = node;
    if (!error.isEmpty()) {
        m_analysisNote = error;
    } else if (kind == QLatin1String(PC::kCutoutType)) {
        QString err;
        m_analysisNote = m_editor->cutoutCompNode(clipId, node, cache, &err) ? T("Fertig") : err;
    } else {
        m_analysisNote = m_editor->trackCompNode(clipId, node, points) ? T("Fertig: %1 Bilder verfolgt").arg(points.size())
                                                                       : T("Das Ergebnis lässt sich nicht speichern");
        if (!note.isEmpty()) m_analysisNote += " – " + note;
    }
    refresh();
    updateAnalysisRow();
}
