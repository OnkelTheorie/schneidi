// Timeline: context menus and renaming tracks in the headers.
#include "ui/timeline/TimelineView.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/I18n.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QContextMenuEvent>
#include <QHash>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <array>
#include <tuple>
#include <cmath>
#include "ui/timeline/TimelineViewDetail.h"

using namespace TimelineViewDetail;

// Rechtsklick auf den Spurkopf (wie DaVinci): sperren, umbenennen, Spurfarbe
void TimelineView::headerMenu(const Row& row, const QPoint& globalPos)
{
    const Timeline& tl = m_editor->project()->timeline();
    const Track& t = tl.track(row.ref);
    const TrackRef ref = row.ref;
    QMenu menu(this);
    QAction* lock = menu.addAction(T("Spur sperren"));
    lock->setCheckable(true);
    lock->setChecked(t.locked);
    connect(lock, &QAction::triggered, this, [this, ref] { m_editor->toggleTrackLock(ref); });
    connect(menu.addAction(T("Spur umbenennen")), &QAction::triggered, this, [this, ref] { startRename(ref); });
    QMenu* colors = menu.addMenu(T("Spurfarbe ändern"));
    auto addColor = [&](const QString& id, const QString& name, const QColor& c) {
        QPixmap pm(12, 12);
        pm.fill(c);
        QAction* a = colors->addAction(QIcon(pm), name);
        a->setCheckable(true);
        a->setChecked(t.color == id);
        connect(a, &QAction::triggered, this, [this, ref, id] { m_editor->setTrackColor(ref, id); });
    };
    addColor({}, T("Standard"), ref.kind == TrackKind::Video ? Theme::videoClip : Theme::audioClip);
    colors->addSeparator();
    for (const auto& i : kTrackColors) addColor(QString::fromLatin1(i.id), T(i.name), QColor::fromRgba(i.rgb));
    // Spuren hinzufügen/löschen (DaVinci „Add Track“/„Delete Track“). Oberhalb/unterhalb wie angezeigt:
    // Video zählt von unten (V1 unten), Audio von oben (A1 oben)
    menu.addSeparator();
    const int above = ref.kind == TrackKind::Video ? ref.index + 1 : ref.index;
    const int below = ref.kind == TrackKind::Video ? ref.index : ref.index + 1;
    connect(menu.addAction(T("Spur oberhalb hinzufügen")), &QAction::triggered, this,
            [this, ref, above] { m_editor->addTrack(ref.kind, above); });
    connect(menu.addAction(T("Spur unterhalb hinzufügen")), &QAction::triggered, this,
            [this, ref, below] { m_editor->addTrack(ref.kind, below); });
    QAction* del = menu.addAction(T("Spur löschen"));
    del->setEnabled(m_editor->canRemoveTrack(ref));
    if (!del->isEnabled())
        del->setToolTip(t.locked ? T("Gesperrte Spuren lassen sich nicht löschen")
                                 : T("Die letzte Spur eines Typs lässt sich nicht löschen"));
    menu.setToolTipsVisible(true);
    connect(del, &QAction::triggered, this, [this, ref] { m_editor->removeTrack(ref); });
    menu.exec(globalPos);
}

// Rechtsklick auf die freie Fläche ohne Spur: neue Spur ans Ende (Video oben, Audio unten), wie DaVinci „Add Track“
void TimelineView::emptyAreaMenu(const QPoint& globalPos)
{
    const Timeline& tl = m_editor->project()->timeline();
    QMenu menu(this);
    connect(menu.addAction(T("Videospur hinzufügen")), &QAction::triggered, this,
            [this, n = int(tl.video.size())] { m_editor->addTrack(TrackKind::Video, n); });
    connect(menu.addAction(T("Audiospur hinzufügen")), &QAction::triggered, this,
            [this, n = int(tl.audio.size())] { m_editor->addTrack(TrackKind::Audio, n); });
    menu.exec(globalPos);
}

void TimelineView::startRename(TrackRef ref)
{
    if (!m_nameEdit) {
        m_nameEdit = new QLineEdit(this);
        m_nameEdit->setFrame(false);
        m_nameEdit->setStyleSheet(QString("QLineEdit { background: %1; color: %2; border: 1px solid %3; padding: 0 2px; }")
                                      .arg(Theme::panel.name(), Theme::text.name(), Theme::primary.name()));
        m_nameEdit->installEventFilter(this);
        connect(m_nameEdit, &QLineEdit::editingFinished, this, [this] { finishRename(true); });
    }
    const auto row = rowFor(ref);
    if (!row) return;
    m_nameSub = -1;
    m_nameRef = ref;
    m_renaming = true;
    QFont f = font();
    f.setPointSizeF(8.5);
    m_nameEdit->setFont(f);
    m_nameEdit->setText(trackDisplayName(m_editor->project()->timeline().track(ref), ref));
    m_nameEdit->setGeometry(nameRect(*row).adjusted(-3, 0, 0, 0));
    m_nameEdit->show();
    m_nameEdit->selectAll();
    m_nameEdit->setFocus();
    update();
}

void TimelineView::finishRename(bool commit)
{
    if (!m_renaming) return;
    m_renaming = false; // vor hide(): Fokusverlust meldet sonst noch einmal editingFinished
    const QString text = m_nameEdit->text();
    m_nameEdit->hide();
    setFocus();
    if (commit && m_nameSub >= 0) m_editor->renameSubtitleTrack(m_nameSub, text);
    else if (commit) m_editor->renameTrack(m_nameRef, text);
    m_nameSub = -1;
    update();
}

bool TimelineView::eventFilter(QObject* obj, QEvent* e)
{
    // Esc im Namensfeld bricht ab (vorher als ShortcutOverride annehmen, sonst greift ein Tastenkürzel)
    if (obj == m_nameEdit && (e->type() == QEvent::ShortcutOverride || e->type() == QEvent::KeyPress)
        && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
        e->accept();
        if (e->type() == QEvent::KeyPress) finishRename(false);
        return true;
    }
    return QWidget::eventFilter(obj, e);
}

// ---------- Maus ----------

// Rechtsklick auf einen Übergang: Art, Ausrichtung, Löschen (wie DaVinci)
void TimelineView::contextMenuEvent(QContextMenuEvent* e)
{
    if (const auto sub = subRowAt(e->pos().y())) {
        if (e->pos().x() < kHeaderW) subtitleHeaderMenu(sub->index, e->globalPos());
        else subtitleTrackMenu(e->pos(), e->globalPos());
        return;
    }
    if (e->pos().x() < kHeaderW && e->pos().y() >= kRulerH) {
        if (const auto row = rowAt(e->pos().y())) headerMenu(*row, e->globalPos());
        else emptyAreaMenu(e->globalPos());
        return;
    }
    // Rechtsklick in der Retime-Leiste: Speed-Punkt (Übergang, Entfernen) bzw. Abschnitt (Tempo)
    if (const auto h = retimeHitAt(e->pos())) {
        if (h->point >= 0) speedPointMenu(*h, e->globalPos());
        else segmentMenu(*h, int(std::lround(xToFrame(e->pos().x()))), e->globalPos());
        return;
    }
    if (curveContextMenu(e->pos(), e->globalPos())) return;
    // Rechtsklick auf eine Keyframe-Raute: Verlauf (wie DaVinci) oder Löschen, gilt für die ausgewählten Rauten
    if (const auto k = keyframeAt(e->pos())) {
        Selection* sel = m_editor->selection();
        if (sel->keyClip() != k->clipId || sel->keyParam() >= 0 || !sel->keyTimes().contains(k->t))
            sel->setKeyframes(k->clipId, {k->t});
        const QVector<int> times = sel->keyTimes().values().toVector();
        const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), k->clipId);
        if (!c) return;
        std::optional<KeyEase> current;
        for (const KeyTrack& track : c->keys)
            for (const Keyframe& kf : track)
                if (kf.frame == c->in + k->t) current = kf.ease;
        QMenu menu(this);
        const struct { KeyEase ease; const char* name; } eases[] = {
            {KeyEase::Linear, "Linear"}, {KeyEase::EaseIn, "Ease In"}, {KeyEase::EaseOut, "Ease Out"},
            {KeyEase::EaseInOut, "Ease In and Out"}, {KeyEase::Bezier, "Bezier"}};
        for (const auto& it : eases) {
            QAction* a = menu.addAction(it.name); // wie DaVinci auch deutsch englisch
            a->setCheckable(true);
            a->setChecked(current == it.ease);
            connect(a, &QAction::triggered, this, [this, id = k->clipId, times, ease = it.ease] {
                m_editor->modifyClips({id}, T("Keyframe-Verlauf"), [&](Clip& clip) { Keys::setEase(clip, times, ease); });
            });
        }
        menu.addSeparator();
        connect(menu.addAction(T("Löschen")), &QAction::triggered, this,
                [this, id = k->clipId, times] { m_editor->removeKeyframes(id, times); });
        menu.exec(e->globalPos());
        return;
    }
    const auto t = transitionAt(e->pos());
    if (!t) {
        // Rechtsklick auf einen Clip: auswählen (falls nicht schon), Menü baut das Hauptfenster (Aktionen)
        if (const int id = clipAt(e->pos())) {
            Selection* sel = m_editor->selection();
            if (!sel->contains(id)) {
                const QVector<int> group = m_editor->withLinked({id});
                sel->set(QSet<int>(group.begin(), group.end()));
            }
            emit clipMenuRequested(e->globalPos());
        } else if (e->pos().y() >= kRulerH && !rowAt(e->pos().y())) {
            emptyAreaMenu(e->globalPos()); // freie Fläche unter/zwischen den Spuren
        }
        return;
    }
    const TimelineOps::TransitionSpan s = t->span;
    m_editor->selection()->setTransition({s.leftId, s.rightId});
    QMenu menu(this);
    if (t->ref.kind == TrackKind::Video) {
        for (const auto& i : kTransitionTypes) {
            QAction* a = menu.addAction(T(i.name));
            a->setCheckable(true);
            a->setChecked(s.style.type == i.type);
            connect(a, &QAction::triggered, this, [this, s, type = i.type] {
                TransitionStyle st = s.style;
                st.type = type;
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
        menu.addSeparator();
    } else {
        for (const auto& i : kAudioCurves) {
            QAction* a = menu.addAction(i.name);
            a->setCheckable(true);
            a->setChecked(s.style.audio == i.curve);
            connect(a, &QAction::triggered, this, [this, s, curve = i.curve] {
                TransitionStyle st = s.style;
                st.audio = curve;
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
        menu.addSeparator();
    }
    if (s.isDissolve()) {
        QMenu* align = menu.addMenu(T("Ausrichtung"));
        const QString names[] = {T("Mitte auf Schnitt"), T("Beginn am Schnitt"), T("Ende am Schnitt")}; // Index = TransitionAlign
        for (int i = 0; i < 3; ++i) {
            QAction* a = align->addAction(names[i]);
            a->setCheckable(true);
            a->setChecked(int(s.style.align) == i);
            connect(a, &QAction::triggered, this, [this, s, i] {
                TransitionStyle st = s.style;
                st.align = TransitionAlign(i);
                m_editor->setTransitionStyle(s.leftId, s.rightId, st);
            });
        }
    }
    connect(menu.addAction(T("Löschen")), &QAction::triggered, this,
            [this, s] { m_editor->removeTransition(s.leftId, s.rightId); });
    menu.exec(e->globalPos());
}
