// SPDX-License-Identifier: AGPL-3.0-only
// Timeline view: story sections and notes of the timeline (0.17 package "story"). Sections are a band in the upper
// part of the ruler (color by kind, from the theme), their edges can be dragged; notes are marks at the bottom of the
// ruler (open: bright, done: faint), the text in the tooltip. Right-click in the ruler: section here (by kind),
// remove the section, a note here, tick off or remove a note. Everything goes through the Editor (one undo step each).
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Story.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "core/TimelineStory.h"
#include "ui/timeline/TimelineView.h"

#include <QHelpEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kEdgePx = 4;  // grab distance of a section edge
constexpr int kNotePx = 5;  // half width of a note mark

QColor kindColor(StoryKind k)
{
    switch (k) {
    case StoryKind::Hook: return Theme::primary;
    case StoryKind::Build: return Theme::videoClip;
    case StoryKind::Question: return Theme::meterMid;
    case StoryKind::Turn: return Theme::titleClip;
    case StoryKind::Resolution: return Theme::audioClip;
    case StoryKind::Cta: return Theme::secondary;
    case StoryKind::Free: break;
    }
    return Theme::controlOff;
}

const char* kindLabel(StoryKind k)
{
    switch (k) {
    case StoryKind::Hook: return N_("Hook");
    case StoryKind::Build: return N_("Aufbau");
    case StoryKind::Question: return N_("Frage");
    case StoryKind::Turn: return N_("Wendung");
    case StoryKind::Resolution: return N_("Auflösung");
    case StoryKind::Cta: return N_("Aufruf");
    case StoryKind::Free: break;
    }
    return N_("Frei");
}

} // namespace

void TimelineView::drawStory(QPainter& p)
{
    const Timeline& tl = m_editor->project()->timeline();
    for (const StorySection& s : tl.sections) {
        const double x0 = std::max<double>(kHeaderW, frameToX(s.from)), x1 = std::min<double>(width(), frameToX(s.to));
        if (x1 <= x0) continue;
        const QColor c = kindColor(s.kind);
        p.fillRect(QRectF(x0, kSectionTop, x1 - x0, kSectionBottom - kSectionTop), Theme::alpha(c, 55));
        p.fillRect(QRectF(x0, kSectionBottom - 2, x1 - x0, 2), c);
        p.fillRect(QRectF(frameToX(s.from), kSectionTop, 1, kSectionBottom - kSectionTop), c);
    }
    for (const TimelineNote& n : tl.notes) {
        const double x = frameToX(n.at);
        if (x < kHeaderW - kNotePx || x > width() + kNotePx) continue;
        QPainterPath mark; // a small speech bubble
        mark.addRoundedRect(QRectF(x - kNotePx, kRulerH - 13, 2 * kNotePx, 7), 2, 2);
        mark.moveTo(x - 2, kRulerH - 6);
        mark.lineTo(x, kRulerH - 2);
        mark.lineTo(x + 2, kRulerH - 6);
        p.fillPath(mark, n.done ? Theme::textFaint : Theme::meterMid);
    }
}

int TimelineView::sectionAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kSectionTop || pos.y() >= kSectionBottom) return -1;
    return Story::at(m_editor->project()->timeline().sections, int(std::floor(xToFrame(pos.x()))));
}

int TimelineView::sectionEdgeAt(const QPoint& pos, bool* left) const
{
    if (pos.x() < kHeaderW || pos.y() < kSectionTop || pos.y() >= kSectionBottom) return -1;
    const QVector<StorySection>& sections = m_editor->project()->timeline().sections;
    int best = -1;
    double dist = kEdgePx + 1;
    for (int i = 0; i < sections.size(); ++i)
        for (const bool l : {true, false}) {
            const double d = std::abs(frameToX(l ? sections[i].from : sections[i].to) - pos.x());
            if (d < dist) {
                dist = d;
                best = i;
                if (left) *left = l;
            }
        }
    return best;
}

int TimelineView::noteAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kSectionBottom || pos.y() >= kRulerH) return 0;
    for (const TimelineNote& n : m_editor->project()->timeline().notes)
        if (std::abs(frameToX(n.at) - pos.x()) <= kNotePx) return n.id;
    return 0;
}

QString TimelineView::storyToolTip(const QPoint& pos) const
{
    const Timeline& tl = m_editor->project()->timeline();
    const int fps = m_editor->project()->fps();
    if (const int id = noteAt(pos))
        for (const TimelineNote& n : tl.notes)
            if (n.id == id)
                return (n.done ? T("Notiz (erledigt)") : T("Notiz")) + QString(" %1: %2").arg(Timecode::format(n.at, fps), n.text);
    if (const int i = sectionAt(pos); i >= 0) {
        const StorySection& s = tl.sections[i];
        QString t = T(kindLabel(s.kind));
        if (!s.name.isEmpty()) t += " – " + s.name;
        return t + QString(" (%1 – %2)").arg(Timecode::format(s.from, fps), Timecode::format(s.to, fps));
    }
    return {};
}

bool TimelineView::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(e);
        if (const QString tip = storyToolTip(help->pos()); !tip.isEmpty()) QToolTip::showText(help->globalPos(), tip, this);
        else QToolTip::hideText();
        return true;
    }
    return QWidget::event(e);
}

bool TimelineView::rulerMenu(const QPoint& pos, const QPoint& globalPos)
{
    if (pos.y() >= kRulerH || pos.x() < kHeaderW) return false;
    const Timeline& tl = m_editor->project()->timeline();
    const int frame = std::max(0, int(std::floor(xToFrame(pos.x()))));
    const int fps = std::max(1, m_editor->project()->fps());
    QMenu menu(this);
    const int section = Story::at(tl.sections, frame);
    if (section < 0) {
        // A new section from here: 2 s, up to the next section or the end of the timeline
        int to = frame + 2 * fps;
        for (const StorySection& s : tl.sections)
            if (s.from > frame) to = std::min(to, s.from);
        if (const int end = TimelineOps::endFrame(tl); end > frame) to = std::min(to, end);
        QMenu* add = menu.addMenu(T("Abschnitt hier"));
        for (const QString& name : TimelineStory::kindNames()) {
            const StoryKind kind = *TimelineStory::kindFromName(name);
            connect(add->addAction(T(kindLabel(kind))), &QAction::triggered, this,
                    [this, kind, frame, to] { m_editor->addSection(StorySection{kind, {}, frame, std::max(frame + 1, to)}); });
        }
    } else {
        connect(menu.addAction(T("Abschnitt entfernen")), &QAction::triggered, this,
                [this, frame] { m_editor->removeSectionAt(frame); });
    }
    menu.addSeparator();
    if (const int id = noteAt(pos)) {
        bool done = false;
        for (const TimelineNote& n : tl.notes)
            if (n.id == id) done = n.done;
        connect(menu.addAction(done ? T("Notiz wieder offen") : T("Notiz erledigt")), &QAction::triggered, this,
                [this, id, done] { m_editor->setNoteDone(id, !done); });
        connect(menu.addAction(T("Notiz entfernen")), &QAction::triggered, this, [this, id] { m_editor->removeNote(id); });
    } else {
        connect(menu.addAction(T("Notiz hier…")), &QAction::triggered, this, [this, frame] {
            bool ok = false;
            const QString text = QInputDialog::getText(this, T("Notiz"), T("Notiz bei %1:").arg(Timecode::format(frame, m_editor->project()->fps())),
                                                       QLineEdit::Normal, {}, &ok);
            if (ok && !text.trimmed().isEmpty()) m_editor->addNote(frame, text.trimmed());
        });
    }
    menu.exec(globalPos);
    return true;
}

bool TimelineView::sectionPress(const QPoint& pos)
{
    bool left = false;
    const int i = sectionEdgeAt(pos, &left);
    if (i < 0) return false;
    m_sectionDrag = i;
    m_sectionLeft = left;
    ++m_sectionDrags;
    m_drag = Drag::SectionEdge;
    return true;
}

void TimelineView::sectionDrag(const QPoint& pos)
{
    const QVector<StorySection>& sections = m_editor->project()->timeline().sections;
    if (m_sectionDrag < 0 || m_sectionDrag >= sections.size()) return;
    const StorySection s = sections[m_sectionDrag];
    int frame = std::max(0, int(std::lround(xToFrame(pos.x()))));
    frame += snapDelta({frame}, {});
    if (m_sectionLeft) frame = std::min(frame, s.to - 1);
    else frame = std::max(frame, s.from + 1);
    // Up to the neighbor (sections do not overlap): a refused range keeps the last one
    m_editor->setSectionRange(m_sectionDrag, m_sectionLeft ? frame : s.from, m_sectionLeft ? s.to : frame,
                              QString("section-drag-%1").arg(m_sectionDrags));
}
