// Inspector: building blocks (sections, effect sections, sliders, XY fields, keyframe buttons).
#include "ui/Inspector.h"

#include "app/Theme.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Stabilizer.h"
#include "ui/ScrubField.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDir>
#include <QComboBox>
#include <QFontComboBox>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <utility>
#include "ui/InspectorDetail.h"

using namespace InspectorDetail;

QToolButton* Inspector::resetButton(const std::function<void()>& fn)
{
    auto* b = new QToolButton;
    b->setText("↺");
    b->setToolTip(T("Zurücksetzen"));
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(QString("QToolButton { color: %1; }").arg(Theme::textDim.name()));
    connect(b, &QToolButton::clicked, this, fn);
    return b;
}

QLabel* Inspector::rowLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->setStyleSheet(QString("color: %1;").arg(Theme::textDim.name()));
    return l;
}

QWidget* Inspector::keyButtons(TrackKind kind, bool title, const QVector<AnimParam>& params, const QString& effect)
{
    auto* box = new QWidget;
    auto* lay = new QHBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto button = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(10, 18);
        lay->addWidget(b);
        return b;
    };
    QToolButton* prev = button("◀", T("Voriger Keyframe"));
    QToolButton* diamond = button("◆", T("Keyframe setzen/entfernen (Rechtsklick: Verlauf)"));
    QToolButton* next = button("▶", T("Nächster Keyframe"));
    diamond->setFixedSize(14, 18);
    const QString arrowCss = QString("QToolButton { color: %1; border: none; padding: 0px; font-size: 6pt; }"
                                     "QToolButton:disabled { color: transparent; }").arg(Theme::textDim.name());
    prev->setStyleSheet(arrowCss);
    next->setStyleSheet(arrowCss);

    // Keyframe at the playhead for all parameters of the row?
    auto onKey = [this, params](const Clip& c) {
        return std::all_of(params.begin(), params.end(), [&](AnimParam p) { return Keys::keyAt(c, p, localFrame(c)); });
    };
    connect(diamond, &QToolButton::clicked, this, [=, this] {
        const Clip* c = primary(kind, title, effect);
        if (!c) return;
        m_editor->setKeyframes(selectedIds(kind, title, effect), params, m_playhead, !onKey(*c));
    });
    // Jumping: only keyframes inside the clip (after trimming some may lie outside)
    auto jump = [=, this](bool forward) {
        const Clip* c = primary(kind, title, effect);
        if (!c) return;
        const int t = localFrame(*c);
        std::optional<int> best;
        for (int k : Keys::keyTimes(*c, params)) {
            if (k < 0 || k >= c->length()) continue;
            if (forward ? k > t && (!best || k < *best) : k < t && (!best || k > *best)) best = k;
        }
        if (best) emit seekRequested(c->start + *best);
    };
    connect(prev, &QToolButton::clicked, this, [jump] { jump(false); });
    connect(next, &QToolButton::clicked, this, [jump] { jump(true); });

    // Right-click on the diamond: interpolation like DaVinci
    diamond->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(diamond, &QWidget::customContextMenuRequested, this, [=, this](const QPoint& pos) {
        const Clip* c = primary(kind, title, effect);
        if (!c || !onKey(*c)) return;
        const Keyframe* k = Keys::keyAt(*c, params.first(), localFrame(*c));
        QMenu menu(this);
        for (const auto& e : kKeyEases) {
            QAction* a = menu.addAction(T(e.name));
            a->setCheckable(true);
            a->setChecked(k && k->ease == e.ease);
            connect(a, &QAction::triggered, this, [=, this, ease = e.ease] {
                m_editor->setKeyframeEase(selectedIds(kind, title, effect), params, m_playhead, ease);
            });
        }
        menu.exec(diamond->mapToGlobal(pos));
    });

    m_refreshers << [=, this] {
        const Clip* c = primary(kind, title, effect);
        if (!c) return;
        const bool animated = std::any_of(params.begin(), params.end(), [&](AnimParam p) { return Keys::animated(*c, p); });
        const bool here = animated && onKey(*c);
        // Diamond: empty = no keyframe here, filled red = keyframe at the playhead (like DaVinci)
        const QString text = here ? "◆" : "◇";
        const QString css = QString("QToolButton { color: %1; border: none; padding: 0px; font-size: 10pt; }")
                                .arg((here ? Theme::warning : animated ? Theme::text : Theme::textDim).name());
        if (diamond->text() != text) diamond->setText(text);
        if (diamond->styleSheet() != css) diamond->setStyleSheet(css);
        const int t = localFrame(*c);
        bool before = false, after = false;
        for (int k : Keys::keyTimes(*c, params)) {
            if (k < 0 || k >= c->length()) continue;
            before |= k < t;
            after |= k > t;
        }
        prev->setEnabled(before);
        next->setEnabled(after);
    };
    return box;
}

Inspector::Section Inspector::addSection(QVBoxLayout* page, TrackKind kind, const QString& title,
                                         const std::function<void(Clip&)>& reset, const Flag& enabled, bool titleOnly,
                                         const QString& effect)
{
    auto* header = new QWidget;
    header->setObjectName("InspectorSection");
    header->setStyleSheet(QString("QWidget#InspectorSection { background: %1; border-top: 1px solid %2;"
                                  " border-bottom: 1px solid %2; }")
                              .arg(Theme::panelHeader.name(), Theme::border.name()));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(10, 3, 4, 3);
    hl->setSpacing(8);

    if (enabled) {
        QToolButton* dot = makeDot();
        connect(dot, &QToolButton::clicked, this, [this, kind, title, enabled, titleOnly, effect](bool on) {
            apply(kind, {}, (on ? T("%1 an") : T("%1 aus")).arg(title), [enabled, on](Clip& c) { enabled(c) = on; },
                  titleOnly, effect);
        });
        m_refreshers << [this, dot, kind, enabled, titleOnly, effect] {
            if (const Clip* c = primary(kind, titleOnly, effect)) {
                Clip copy = *c;
                const QSignalBlocker b(dot);
                dot->setChecked(enabled(copy));
            }
        };
        hl->addWidget(dot);
    } else {
        hl->addSpacing(10);
    }

    auto* toggle = new QToolButton;
    toggle->setText(title);
    toggle->setCheckable(true);
    toggle->setChecked(true);
    toggle->setToolTip(T("Auf-/zuklappen"));
    toggle->setFocusPolicy(Qt::NoFocus);
    toggle->setStyleSheet(QString("QToolButton, QToolButton:checked { color: %1; border: none; background: transparent;"
                                  " font-weight: 600; }")
                              .arg(Theme::text.name()));
    hl->addWidget(toggle);
    hl->addStretch(1);
    if (reset)
        hl->addWidget(resetButton([this, kind, title, reset, titleOnly, effect] {
            apply(kind, {}, T("%1 zurücksetzen").arg(title), reset, titleOnly, effect);
        }));

    auto* body = new QWidget;
    auto* grid = new QGridLayout(body);
    grid->setContentsMargins(10, 6, 4, 8);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);
    grid->setColumnMinimumWidth(0, 72);
    grid->setColumnStretch(1, 1);
    connect(toggle, &QToolButton::toggled, body, [body](bool open) {
        body->setProperty("collapsed", !open);
        body->setVisible(open);
    });

    page->addWidget(header);
    page->addWidget(body);
    return {grid, kind, 0, titleOnly, effect, header, body, hl, toggle};
}

void Inspector::addEffectSection(QVBoxLayout* page, const EffectDescriptor& d)
{
    const QString id = d.id;
    const TrackKind kind = d.video ? TrackKind::Video : TrackKind::Audio;
    Section s = addSection(page, kind, d.name, [id](Clip& c) {
        // Reset: default values, keyframes gone, switched on
        EffectInstance* e = EffectRegistry::instance(c, id);
        const EffectDescriptor* desc = EffectRegistry::find(id);
        if (!e || !desc) return;
        e->enabled = true;
        for (const auto& p : desc->params) {
            e->params[p.key] = p.defaultValue;
            if (p.anim != AnimParam::Count) Keys::clear(c, p.anim);
        }
    }, [id](Clip& c) -> bool& {
        static bool none;
        EffectInstance* e = EffectRegistry::instance(c, id);
        return e ? e->enabled : none;
    }, false, id);

    addRemoveButton(s, id);
    addMoveHandle(s, id);

    if (!d.description.isEmpty()) s.header->setToolTip(d.description);
    // Elide long names (frei0r), full name + description in the tooltip
    auto label = [this](const EffectParam& p) {
        QLabel* l = rowLabel(p.label);
        const QString shown = l->fontMetrics().elidedText(p.label, Qt::ElideRight, 110);
        l->setText(shown);
        if (shown != p.label || !p.description.isEmpty())
            l->setToolTip(p.description.isEmpty() || p.description == p.label ? p.label
                                                                               : p.label + "\n" + p.description);
        return l;
    };
    // Row with its own control (checkbox, color, choice) and a reset button
    auto addRow = [&s, &label](const EffectParam& p, QWidget* w, QToolButton* reset) {
        auto* box = new QHBoxLayout;
        box->setContentsMargins(0, 0, 0, 0);
        if (qobject_cast<QToolButton*>(w)) box->addStretch(1); // color swatch right-aligned like everywhere
        box->addWidget(w, qobject_cast<QComboBox*>(w) ? 1 : 0);
        if (qobject_cast<QCheckBox*>(w)) box->addStretch(1);
        s.grid->addWidget(label(p), s.rows, 0);
        s.grid->addLayout(box, s.rows, 1);
        s.grid->addWidget(reset, s.rows, 2);
        ++s.rows;
    };
    // Set a value: one undo step per change
    auto set = [this, id, kind](const EffectParam& p, const QVariant& v) {
        apply(kind, {}, p.label, [id, key = p.key, v](Clip& c) {
            if (EffectInstance* e = EffectRegistry::instance(c, id)) e->params[key] = v;
        }, false, id);
    };

    for (const EffectParam& p : d.params) {
        if (p.type == EffectParam::Bool) {
            auto* box = new QCheckBox;
            box->setFocusPolicy(Qt::NoFocus);
            connect(box, &QCheckBox::clicked, this, [set, p](bool on) { set(p, on); });
            addRow(p, box, resetButton([set, p] { set(p, p.defaultValue); }));
            m_refreshers << [this, box, id, kind, key = p.key] {
                if (const Clip* c = primary(kind, false, id)) {
                    const QSignalBlocker b(box);
                    box->setChecked(EffectRegistry::value(*c, id, key).toBool());
                }
            };
            continue;
        }
        if (p.type == EffectParam::Choice) {
            auto* combo = new QComboBox;
            combo->setFocusPolicy(Qt::NoFocus);
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            for (const QString& v : p.choices) combo->addItem(p.choiceText(v), v); // shown text, value as data
            connect(combo, &QComboBox::activated, this, [set, p, combo](int i) { set(p, combo->itemData(i).toString()); });
            addRow(p, combo, resetButton([set, p] { set(p, p.defaultValue); }));
            m_refreshers << [this, combo, id, kind, key = p.key] {
                if (const Clip* c = primary(kind, false, id)) {
                    const QSignalBlocker b(combo);
                    combo->setCurrentIndex(combo->findData(EffectRegistry::value(*c, id, key).toString()));
                }
            };
            continue;
        }
        if (p.type == EffectParam::Color) {
            auto* swatch = new QToolButton;
            swatch->setFixedSize(46, 18);
            swatch->setFocusPolicy(Qt::NoFocus);
            swatch->setToolTip(T("%1 wählen").arg(p.label));
            connect(swatch, &QToolButton::clicked, this, [this, set, p, id, kind] {
                const Clip* c = primary(kind, false, id);
                if (!c) return;
                const QColor col = QColorDialog::getColor(EffectRegistry::value(*c, id, p.key).value<QColor>(), this,
                                                          T("%1 wählen").arg(p.label));
                if (col.isValid()) set(p, col);
            });
            addRow(p, swatch, resetButton([set, p] { set(p, p.defaultValue); }));
            m_refreshers << [this, swatch, id, kind, key = p.key] {
                if (const Clip* c = primary(kind, false, id)) {
                    const QColor col = EffectRegistry::value(*c, id, key).value<QColor>();
                    const QString css = QString("QToolButton { background: %1; border: 1px solid %2; border-radius: 2px; }")
                                            .arg(col.name(), Theme::border.name());
                    if (swatch->styleSheet() != css) swatch->setStyleSheet(css);
                }
            };
            continue;
        }
        if (p.type != EffectParam::Double) continue; // paths (LUT) only on the Color page
        const int row = s.rows;
        std::optional<AnimParam> anim;
        if (p.anim != AnimParam::Count) anim = p.anim;
        const QString key = p.key;
        Param* param = addSlider(s, "fx:" + id + ":" + key, p.label, p.min, p.max, p.defaultValue.toDouble(), p.step,
                                 p.decimals, [id, key](Clip& c) -> double& {
                                     static double shown;
                                     shown = EffectRegistry::value(c, id, key).toDouble();
                                     return shown;
                                 }, anim);
        if (!anim) // not animatable: write directly into the instance
            param->edit->onChange = [this, param, id, key, kind, label = p.label](double v) {
                const QSignalBlocker b(param->slider);
                param->slider->setValue(sliderPos(v, param->min, param->max));
                apply(kind, "fx:" + id + ":" + key, label, [id, key, v](Clip& c) {
                    if (EffectInstance* e = EffectRegistry::instance(c, id)) e->params[key] = v;
                }, false, id);
            };
        if (QLayoutItem* item = s.grid->itemAtPosition(row, 0); item && item->widget()) {
            QLabel* l = label(p);
            delete item->widget();
            s.grid->addWidget(l, row, 0);
        }
    }
    if (id == Stabilizer::EffectId) addStabilizeRow(s);

    s.header->hide();
    s.body->hide();
    m_fxSections << FxSection{id, s.header, s.body, kind};
}

void Inspector::addStabilizeRow(Section& s)
{
    // Like DaVinci's "Stabilize" button, but automatic: the analysis starts when the effect is added or an analysis
    // parameter changes; here its state, cancel and restart
    auto* status = new QLabel;
    status->setWordWrap(true);
    status->setObjectName("StabilizeStatus");
    auto* button = new QPushButton;
    button->setObjectName("StabilizeButton");
    button->setFocusPolicy(Qt::NoFocus);
    s.grid->addWidget(rowLabel(T("Analyse")), s.rows, 0);
    s.grid->addWidget(status, s.rows, 1);
    s.grid->addWidget(button, ++s.rows, 1, Qt::AlignLeft);
    ++s.rows;
    auto current = [this]() -> std::optional<Stabilizer::Request> {
        const Clip* c = primary(TrackKind::Video, false, Stabilizer::EffectId);
        return c ? Stabilizer::request(*c, m_editor->project()->format()) : std::nullopt;
    };
    connect(button, &QPushButton::clicked, this, [current] {
        const auto r = current();
        if (!r) return;
        Stabilizer* st = Stabilizer::instance();
        const Stabilizer::State state = st->state(*r);
        if (state == Stabilizer::State::Running || state == Stabilizer::State::Queued) st->cancel(*r);
        else st->start(*r);
    });
    m_stabilizeRefresh = [this, status, button, current] {
        const Clip* c = primary(TrackKind::Video, false, Stabilizer::EffectId);
        if (!c) return;
        QString text;
        QString action;
        QColor color = Theme::textDim;
        const auto r = current();
        if (!r) {
            text = Stabilizer::unsupportedReason(*c);
            color = Theme::warning;
        } else {
            double progress = 0;
            switch (Stabilizer::instance()->state(*r, &progress)) {
            case Stabilizer::State::Done: text = T("Fertig"); break;
            case Stabilizer::State::Running:
                text = T("Analysiert … %1 %").arg(int(progress * 100));
                action = T("Abbrechen");
                break;
            case Stabilizer::State::Queued: text = T("Wartet auf die Analyse"); action = T("Abbrechen"); break;
            case Stabilizer::State::Cancelled:
                text = T("Abgebrochen – der Clip läuft unstabilisiert");
                action = T("Analysieren");
                color = Theme::warning;
                break;
            case Stabilizer::State::Failed:
                text = Stabilizer::instance()->error(*r);
                action = T("Erneut versuchen");
                color = Theme::warning;
                break;
            case Stabilizer::State::Missing:
            case Stabilizer::State::Unsupported: text = T("Noch nicht analysiert"); action = T("Analysieren"); break;
            }
        }
        status->setText(text);
        status->setStyleSheet(QString("color: %1;").arg(color.name()));
        button->setVisible(!action.isEmpty());
        button->setText(action);
    };
    m_refreshers << m_stabilizeRefresh;
    connect(Stabilizer::instance(), &Stabilizer::stateChanged, this, [this] {
        if (m_stabilizeRefresh) m_stabilizeRefresh();
    });
}

void Inspector::addRemoveButton(const Section& s, const QString& id)
{
    // Remove: trash can in the header or right-click on the header
    auto remove = [this, id, kind = s.kind] { m_editor->removeEffect(selectedIds(kind, false, id), id); };
    auto* trash = new QToolButton;
    trash->setIcon(trashIcon());
    trash->setIconSize(QSize(14, 14));
    trash->setAutoRaise(true);
    trash->setFocusPolicy(Qt::NoFocus);
    trash->setToolTip(T("Effekt entfernen"));
    trash->setStyleSheet("QToolButton { border: none; background: transparent; }");
    connect(trash, &QToolButton::clicked, this, remove);
    s.headerLayout->insertWidget(s.headerLayout->count() - 1, trash); // before the reset button
    s.header->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(s.header, &QWidget::customContextMenuRequested, this,
            [this, header = s.header, remove, id, kind = s.kind, video = s.kind == TrackKind::Video](const QPoint& pos) {
                QMenu menu(this);
                if (EffectRegistry::movable(id)) { // effect order (fixed stages keep their place)
                    int count = 0;
                    const int place = effectPlace(kind, id, &count);
                    QAction* up = menu.addAction(T("Nach oben"));
                    up->setEnabled(place > 0);
                    connect(up, &QAction::triggered, this,
                            [this, id, kind] { m_editor->moveEffectBy(selectedIds(kind, false, id), id, -1); });
                    QAction* down = menu.addAction(T("Nach unten"));
                    down->setEnabled(place >= 0 && place < count - 1);
                    connect(down, &QAction::triggered, this,
                            [this, id, kind] { m_editor->moveEffectBy(selectedIds(kind, false, id), id, 1); });
                    menu.addSeparator();
                }
                connect(menu.addAction(T("Effekt entfernen")), &QAction::triggered, this, remove);
                if (video) { // presets hold video effects
                    menu.addSeparator();
                    connect(menu.addAction(T("Effekte als Preset speichern…")), &QAction::triggered, this,
                            &Inspector::savePresetRequested);
                }
                menu.exec(header->mapToGlobal(pos));
            });
}

void Inspector::addMoveHandle(const Section& s, const QString& id)
{
    auto* grip = new QLabel;
    grip->setObjectName("EffectGrip");
    grip->setFixedSize(8, 14);
    if (!EffectRegistry::movable(id)) { // same indent as the others, nothing to drag
        s.headerLayout->insertWidget(0, grip);
        return;
    }
    grip->setPixmap(gripPixmap(devicePixelRatioF()));
    grip->setCursor(Qt::OpenHandCursor);
    grip->setToolTip(T("Ziehen ändert die Reihenfolge: oben wird zuerst berechnet"));
    s.headerLayout->insertWidget(0, grip);
    for (QWidget* w : {s.header, static_cast<QWidget*>(grip), static_cast<QWidget*>(s.toggle)})
        if (w) w->installEventFilter(this);
}

int Inspector::effectPlace(TrackKind kind, const QString& id, int* count) const
{
    const Clip* c = primary(kind, false, id);
    const QStringList order = c ? EffectRegistry::order(*c) : QStringList();
    if (count) *count = int(order.size());
    return int(order.indexOf(id));
}

bool Inspector::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease)
        return QWidget::eventFilter(watched, event);
    auto* w = qobject_cast<QWidget*>(watched);
    const FxSection* section = nullptr;
    for (const FxSection& f : std::as_const(m_fxSections))
        if (w && (f.header == w || f.header == w->parentWidget())) section = &f;
    if (!section) return QWidget::eventFilter(watched, event);
    // The title folds the section on a click: it keeps its press and a release without drag
    const bool title = qobject_cast<QToolButton*>(w) != nullptr;
    auto* me = static_cast<QMouseEvent*>(event);
    const QPoint global = me->globalPosition().toPoint();
    if (type == QEvent::MouseButtonPress) {
        if (me->button() != Qt::LeftButton) return false;
        endEffectDrag(false);
        m_fxDrag = FxDrag{section->header, section->id, section->kind, global};
        return !title;
    }
    if (m_fxDrag.header != section->header) return false;
    if (type == QEvent::MouseMove) {
        if (!m_fxDrag.active) {
            if ((global - m_fxDrag.start).manhattanLength() < QApplication::startDragDistance()) return !title;
            m_fxDrag.active = true;
            if (title) static_cast<QToolButton*>(w)->setDown(false); // the release then does not fold it
            m_fxDrag.style = m_fxDrag.header->styleSheet();
            m_fxDrag.header->setStyleSheet(QString("QWidget#InspectorSection { background: %1; border: 1px solid %2; }")
                                               .arg(Theme::control.name(), Theme::primary.name()));
            QApplication::setOverrideCursor(Qt::ClosedHandCursor);
        }
        dragEffect(global);
        return true;
    }
    // Release: the title gets it too (after a drag it is no longer down, so it does not fold)
    endEffectDrag(true);
    return !title;
}

void Inspector::dragEffect(const QPoint& globalPos)
{
    // Gaps between the shown movable sections of the kind; the cursor lands in the one nearest to it
    const Clip* c = primary(m_fxDrag.kind, false, m_fxDrag.id);
    const QStringList order = c ? EffectRegistry::order(*c) : QStringList();
    const int from = int(order.indexOf(m_fxDrag.id));
    QWidget* page = m_fxDrag.header->parentWidget();
    if (from < 0 || !page) return;
    const int y = page->mapFromGlobal(globalPos).y();
    QVector<QPair<int, int>> spans; // top and bottom of each section on the page
    for (const QString& id : order)
        for (const FxSection& f : std::as_const(m_fxSections))
            if (f.kind == m_fxDrag.kind && f.id == id && f.header->isVisible()) {
                const int bottom = f.body->isVisible() ? f.body->geometry().bottom() : f.header->geometry().bottom();
                spans << qMakePair(f.header->geometry().top(), bottom);
            }
    if (spans.size() != order.size()) return;
    int gap = 0;
    for (int k = 0; k < spans.size(); ++k)
        if (y > (spans[k].first + spans[k].second) / 2) gap = k + 1;
    m_fxDrag.to = gap > from ? gap - 1 : gap;
    if (!m_dropLine || m_dropLine->parentWidget() != page) {
        delete m_dropLine;
        m_dropLine = new QWidget(page);
        m_dropLine->setObjectName("EffectDropLine");
        m_dropLine->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_dropLine->setStyleSheet(QString("background: %1;").arg(Theme::primary.name()));
    }
    if (m_fxDrag.to == from) {
        m_dropLine->hide();
        return;
    }
    const int lineY = gap < spans.size() ? spans[gap].first : spans.last().second + 1;
    m_dropLine->setGeometry(0, lineY - 1, page->width(), 3);
    m_dropLine->show();
    m_dropLine->raise();
}

void Inspector::endEffectDrag(bool apply)
{
    FxDrag drag = std::exchange(m_fxDrag, FxDrag{});
    if (m_dropLine) m_dropLine->hide();
    if (!drag.active) return;
    drag.header->setStyleSheet(drag.style);
    QApplication::restoreOverrideCursor();
    if (apply && drag.to >= 0 && drag.to != effectPlace(drag.kind, drag.id))
        m_editor->moveEffect(selectedIds(drag.kind, false, drag.id), drag.id, drag.to);
}

void Inspector::addMissingEffectSection(QVBoxLayout* page, const QString& id, TrackKind kind)
{
    // Like "Media Offline": the effect stays in the project with its values, but nothing renders it
    const QString title = T("%1 (fehlt)").arg(id);
    Section s = addSection(page, kind, title, {}, {}, false, id);
    addRemoveButton(s, id);
    addMoveHandle(s, id);
    for (QToolButton* b : s.header->findChildren<QToolButton*>()) // title in the warning color
        if (b->isCheckable() && b->text() == title)
            b->setStyleSheet(QString("QToolButton, QToolButton:checked { color: %1; border: none; background: transparent;"
                                     " font-weight: 600; }").arg(Theme::warning.name()));
    auto* warn = new QLabel(T("Plugin auf diesem Rechner nicht installiert: der Effekt wirkt nicht, bleibt aber mit "
                              "seinen Werten im Projekt."));
    warn->setObjectName("MissingEffect");
    warn->setWordWrap(true);
    warn->setStyleSheet(QString("color: %1;").arg(Theme::warning.name()));
    s.grid->addWidget(warn, 0, 0, 1, 4);
    s.header->setToolTip(T("Effekt „%1“ fehlt").arg(id));
    s.header->hide();
    s.body->hide();
    m_fxSections << FxSection{id, s.header, s.body, kind};
}

int Inspector::effectCount(const Clip& c) const
{
    // What the Effects tab shows: Effects Library effects, missing plugins and the Color page grade
    int n = ColorGrade::active(c) ? 1 : 0;
    for (const EffectInstance& e : c.effects) {
        const EffectDescriptor* d = EffectRegistry::find(e.effectId);
        if (!d || (d->library && d->video)) ++n;
    }
    return n;
}

void Inspector::arrangeEffectSections()
{
    // Shown are the effects of the shown (earliest selected) clip of each kind, in its order: video effects in the
    // Effects tab, audio effects in the Audio tab
    const Clip* c = primary(TrackKind::Video);
    const bool grade = c && ColorGrade::active(*c);
    m_gradeRow->setVisible(grade);
    if (grade) {
        const QString lut = ColorGrade::lutPath(*c);
        m_gradeText->setText(T("Farbkorrektur der Color-Seite") +
                             (lut.isEmpty() ? QString() : "\n" + T("LUT: %1").arg(QFileInfo(lut).completeBaseName())));
    }
    for (TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const bool video = kind == TrackKind::Video;
        QVBoxLayout* lay = video ? m_fxLay : m_audioFxLay;
        const Clip* shownClip = video ? c : primary(TrackKind::Audio);
        auto known = [&](const QString& id) {
            return std::any_of(m_fxSections.begin(), m_fxSections.end(),
                               [&](const FxSection& f) { return f.kind == kind && f.id == id; });
        };
        QStringList order; // render order: fixed stages (stabilization) first, wherever they are in the list
        if (shownClip)
            for (const EffectInstance* e : EffectRegistry::renderOrder(*shownClip)) {
                const QString& id = e->effectId;
                if (!known(id)) { // create on the first clip with the effect (frei0r: more than a hundred possible)
                    const EffectDescriptor* d = EffectRegistry::find(id);
                    if (d && d->library && d->video == video) addEffectSection(lay, *d);
                    else if (!d) addMissingEffectSection(lay, id, kind); // plugin missing
                }
                if (known(id)) order << id;
            }
        QStringList& last = video ? m_fxOrder : m_audioFxOrder;
        if (order == last) continue;
        last = order;
        int index = video ? m_fxIndex : m_audioFxIndex;
        for (const QString& id : order)
            for (const FxSection& f : m_fxSections)
                if (f.kind == kind && f.id == id) {
                    lay->removeWidget(f.header);
                    lay->removeWidget(f.body);
                    lay->insertWidget(index++, f.header);
                    lay->insertWidget(index++, f.body);
                }
        for (const FxSection& f : m_fxSections) {
            if (f.kind != kind) continue;
            const bool shown = order.contains(f.id);
            f.header->setVisible(shown);
            f.body->setVisible(shown && !f.body->property("collapsed").toBool());
        }
    }
}

Inspector::Param* Inspector::makeParam(const Section& s, const QString& key, const QString& text, double min,
                                       double max, double def, double step, int decimals, const Field& field,
                                       std::optional<AnimParam> anim)
{
    auto* p = new Param{s.kind, s.title, field, new ScrubField(min, max, step, decimals), nullptr, min, max, anim, s.effect};
    p->edit->setValue(def);
    p->edit->onChange = [this, p, key, text, field](double v) {
        if (p->slider) {
            const QSignalBlocker b(p->slider);
            p->slider->setValue(sliderPos(v, p->min, p->max));
        }
        apply(p->kind, key, text, [this, p, field, v](Clip& c) {
            // animated: changing the value sets a keyframe at the playhead (like DaVinci)
            if (p->anim) Keys::setValue(c, *p->anim, localFrame(c), v);
            else field(c) = v;
        }, p->title, p->effect);
    };
    p->edit->onFinish = [this] { m_editor->project()->closeMerge(); };
    m_params << p;
    return p;
}

Inspector::Param* Inspector::addSlider(Section& s, const QString& key, const QString& label, double min, double max,
                                       double def, double step, int decimals, const Field& field,
                                       std::optional<AnimParam> anim)
{
    Param* p = makeParam(s, key, label, min, max, def, step, decimals, field, anim);
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(0, kSliderSteps);
    slider->setFocusPolicy(Qt::NoFocus); // arrow keys stay with the timeline
    slider->setValue(sliderPos(def, min, max));
    p->slider = slider;
    connect(slider, &QSlider::valueChanged, this, [p](int v) {
        p->edit->setValue(p->min + (p->max - p->min) * v / kSliderSteps);
        const QSignalBlocker b(p->slider); // avoid feedback
        if (p->edit->onChange) p->edit->onChange(p->edit->value());
    });
    connect(slider, &QSlider::sliderReleased, this, [this] { m_editor->project()->closeMerge(); });

    auto* box = new QHBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(8);
    box->addWidget(slider, 1);
    box->addWidget(p->edit);
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(box, s.rows, 1);
    s.grid->addWidget(resetButton([p, def] {
        p->edit->setValue(def);
        if (p->edit->onChange) p->edit->onChange(def);
        if (p->edit->onFinish) p->edit->onFinish();
    }), s.rows, 2);
    if (anim) s.grid->addWidget(keyButtons(s.kind, s.title, {*anim}, s.effect), s.rows, 3);
    ++s.rows;
    return p;
}

QPair<Inspector::Param*, Inspector::Param*> Inspector::addXY(Section& s, const QString& key, const QString& label,
                                                             double min, double max, double def, double step,
                                                             int decimals, const Field& x, const Field& y,
                                                             QToolButton* link, std::optional<AnimParam> ax,
                                                             std::optional<AnimParam> ay)
{
    Param* px = makeParam(s, key + "X", label + " X", min, max, def, step, decimals, x, ax);
    Param* py = makeParam(s, key + "Y", label + " Y", min, max, def, step, decimals, y, ay);
    auto* box = new QHBoxLayout;
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(4);
    box->addStretch(1);
    box->addWidget(axisLabel("X"));
    box->addWidget(px->edit);
    if (link) box->addWidget(link);
    else box->addSpacing(20 + box->spacing()); // fields flush with the Zoom row
    box->addWidget(axisLabel("Y"));
    box->addWidget(py->edit);
    s.grid->addWidget(rowLabel(label), s.rows, 0);
    s.grid->addLayout(box, s.rows, 1);
    s.grid->addWidget(resetButton([this, kind = s.kind, title = s.title, label, x, y, def, ax, ay] {
        apply(kind, {}, T("%1 zurücksetzen").arg(label), [this, x, y, def, ax, ay](Clip& c) {
            if (ax) Keys::setValue(c, *ax, localFrame(c), def);
            else x(c) = def;
            if (ay) Keys::setValue(c, *ay, localFrame(c), def);
            else y(c) = def;
        }, title);
    }), s.rows, 2);
    if (ax && ay) s.grid->addWidget(keyButtons(s.kind, s.title, {*ax, *ay}), s.rows, 3);
    ++s.rows;
    return {px, py};
}
