// Editor: Untertitelspuren (Teil von Editor, eigene Datei der Übersicht halber)
#include "core/Editor.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"

#include <QSet>
#include <algorithm>
#include <climits>

namespace {

bool validTrack(const Timeline& tl, int index)
{
    return index >= 0 && index < tl.subtitles.size();
}

// Grenzen für einen Eintrag: Ende des vorigen, Anfang des nächsten (INT_MAX = keiner)
void neighbours(const SubtitleTrack& t, int id, int* lo, int* hi)
{
    *lo = 0;
    *hi = INT_MAX;
    for (int i = 0; i < t.cues.size(); ++i)
        if (t.cues[i].id == id) {
            if (i > 0) *lo = t.cues[i - 1].end;
            if (i + 1 < t.cues.size()) *hi = t.cues[i + 1].start;
        }
}

} // namespace

bool Editor::isSubtitle(int id) const
{
    return Subtitles::find(m_project->timeline(), id) != nullptr;
}

bool Editor::isSubtitleTrackLocked(int index) const
{
    const Timeline& tl = m_project->timeline();
    return validTrack(tl, index) && tl.subtitles[index].locked;
}

QVector<int> Editor::selectedSubtitles() const
{
    QVector<int> out;
    const Timeline& tl = m_project->timeline();
    for (int t = 0; t < tl.subtitles.size(); ++t)
        if (!tl.subtitles[t].locked)
            for (const SubtitleCue& c : tl.subtitles[t].cues)
                if (m_selection->contains(c.id)) out << c.id;
    return out;
}

int Editor::addSubtitleTrack()
{
    const Timeline& cur = m_project->timeline();
    const int index = cur.subtitles.size();
    const bool anyOn = std::any_of(cur.subtitles.begin(), cur.subtitles.end(), [](const SubtitleTrack& t) { return t.enabled; });
    m_project->edit(T("Untertitelspur hinzufügen"), [&](Timeline& tl) {
        SubtitleTrack t;
        t.style = Subtitles::defaultStyle(m_project->format().size());
        t.enabled = !anyOn;
        tl.subtitles << t;
    });
    return index;
}

void Editor::removeSubtitleTrack(int index)
{
    const Timeline& cur = m_project->timeline();
    if (!validTrack(cur, index)) return;
    QSet<int> ids;
    for (const SubtitleCue& c : cur.subtitles[index].cues) ids.insert(c.id);
    if (m_selection->ids().intersects(ids)) m_selection->set(m_selection->ids() - ids);
    m_project->edit(T("Untertitelspur löschen"), [&](Timeline& tl) { tl.subtitles.removeAt(index); });
}

void Editor::setSubtitleTrackEnabled(int index, bool on)
{
    const Timeline& cur = m_project->timeline();
    if (!validTrack(cur, index) || cur.subtitles[index].enabled == on) return;
    m_project->edit(on ? T("Untertitelspur einblenden") : T("Untertitelspur ausblenden"), [&](Timeline& tl) {
        for (int i = 0; i < tl.subtitles.size(); ++i) {
            if (i == index) tl.subtitles[i].enabled = on;
            else if (on) tl.subtitles[i].enabled = false;
        }
    });
}

void Editor::toggleSubtitleTrackLock(int index)
{
    const Timeline& cur = m_project->timeline();
    if (!validTrack(cur, index)) return;
    const bool lock = !cur.subtitles[index].locked;
    if (lock) {
        QSet<int> ids;
        for (const SubtitleCue& c : cur.subtitles[index].cues) ids.insert(c.id);
        if (m_selection->ids().intersects(ids)) m_selection->set(m_selection->ids() - ids);
    }
    m_project->edit(lock ? T("Spur sperren") : T("Spur entsperren"), [&](Timeline& tl) { tl.subtitles[index].locked = lock; });
}

void Editor::renameSubtitleTrack(int index, const QString& name)
{
    const Timeline& cur = m_project->timeline();
    if (!validTrack(cur, index)) return;
    QString n = name.simplified();
    if (n == subtitleTrackDisplayName(SubtitleTrack{}, index)) n.clear();
    if (n == cur.subtitles[index].name) return;
    m_project->edit(T("Spur umbenennen"), [&](Timeline& tl) { tl.subtitles[index].name = n; });
}

void Editor::setSubtitleStyle(int index, const QString& text, const std::function<void(TitleStyle&)>& fn,
                              const QString& mergeKey)
{
    const Timeline& cur = m_project->timeline();
    if (!validTrack(cur, index)) return;
    TitleStyle st = cur.subtitles[index].style;
    fn(st);
    if (st == cur.subtitles[index].style) return;
    m_project->edit(text, [&](Timeline& tl) { tl.subtitles[index].style = st; }, mergeKey);
}

int Editor::addSubtitle(int frame, int track, const QString& text)
{
    frame = std::max(0, frame);
    const Timeline& cur = m_project->timeline();
    if (track < 0) {
        track = 0;
        for (int i = 0; i < cur.subtitles.size(); ++i)
            if (cur.subtitles[i].enabled) track = i;
    }
    const bool create = track >= cur.subtitles.size();
    if (create) track = cur.subtitles.size();
    if (!create && (cur.subtitles[track].locked || Subtitles::cueAt(cur.subtitles[track], frame) >= 0)) return 0;
    int end = frame + 3 * m_project->fps(); // wie DaVinci: 3 s
    if (!create)
        for (const SubtitleCue& c : cur.subtitles[track].cues)
            if (c.start > frame) end = std::min(end, c.start);
    const bool anyOn = std::any_of(cur.subtitles.begin(), cur.subtitles.end(), [](const SubtitleTrack& t) { return t.enabled; });
    const int id = m_project->newClipId();
    m_project->edit(T("Untertitel hinzufügen"), [&](Timeline& tl) {
        while (tl.subtitles.size() <= track) {
            SubtitleTrack t;
            t.style = Subtitles::defaultStyle(m_project->format().size());
            t.enabled = !anyOn;
            tl.subtitles << t;
        }
        Subtitles::place(tl.subtitles[track], SubtitleCue{id, frame, end, text.isNull() ? T("Untertitel") : text});
    });
    m_selection->set({id});
    return id;
}

void Editor::setSubtitleText(int id, const QString& text, const QString& mergeKey)
{
    int track = -1;
    const SubtitleCue* c = Subtitles::find(m_project->timeline(), id, &track);
    if (!c || c->text == text || isSubtitleTrackLocked(track)) return;
    m_project->edit(T("Untertiteltext"), [&](Timeline& tl) {
        if (SubtitleCue* cue = Subtitles::find(tl, id)) cue->text = text;
    }, mergeKey);
}

void Editor::setSubtitleTiming(int id, int start, int end)
{
    int track = -1;
    const Timeline& cur = m_project->timeline();
    const SubtitleCue* c = Subtitles::find(cur, id, &track);
    if (!c || isSubtitleTrackLocked(track)) return;
    int lo, hi;
    neighbours(cur.subtitles[track], id, &lo, &hi);
    start = std::clamp(start, lo, std::min(hi, INT_MAX - 1) - 1);
    end = std::clamp(end, start + 1, hi);
    if (start == c->start && end == c->end) return;
    m_project->edit(T("Untertitel-Zeiten"), [&](Timeline& tl) {
        if (SubtitleCue* cue = Subtitles::find(tl, id)) {
            cue->start = start;
            cue->end = end;
        }
    });
}

int Editor::clampSubtitleTrackDelta(const QVector<int>& ids, int trackDelta) const
{
    const Timeline& tl = m_project->timeline();
    for (int id : ids) {
        int t = -1;
        if (!Subtitles::find(tl, id, &t)) continue;
        trackDelta = std::clamp(trackDelta, -t, int(tl.subtitles.size()) - 1 - t);
    }
    return trackDelta;
}

void Editor::moveSubtitles(const QVector<int>& ids, int delta, int trackDelta)
{
    const Timeline& cur = m_project->timeline();
    QVector<QPair<int, SubtitleCue>> moving; // Zielspur, Eintrag
    int minStart = INT_MAX;
    trackDelta = clampSubtitleTrackDelta(ids, trackDelta);
    for (int id : ids) {
        int t = -1;
        const SubtitleCue* c = Subtitles::find(cur, id, &t);
        if (!c || cur.subtitles[t].locked || (trackDelta && cur.subtitles[t + trackDelta].locked)) continue;
        moving << qMakePair(t + trackDelta, *c);
        minStart = std::min(minStart, c->start);
    }
    if (moving.isEmpty()) return;
    delta = std::max(delta, -minStart);
    if (delta == 0 && trackDelta == 0) return;
    Project* p = m_project;
    m_project->edit(T("Untertitel verschieben"), [&](Timeline& tl) {
        QSet<int> movingIds;
        for (const auto& m : moving) movingIds.insert(m.second.id);
        for (SubtitleTrack& t : tl.subtitles)
            t.cues.erase(std::remove_if(t.cues.begin(), t.cues.end(),
                                        [&](const SubtitleCue& c) { return movingIds.contains(c.id); }),
                         t.cues.end());
        for (auto m : moving) {
            m.second.start += delta;
            m.second.end += delta;
            Subtitles::place(tl.subtitles[m.first], m.second);
        }
        for (SubtitleTrack& t : tl.subtitles) // geteilte Einträge bekommen eine eigene id
            for (SubtitleCue& c : t.cues)
                if (c.id == 0) c.id = p->newClipId();
    });
}

int Editor::clampSubtitleTrim(int id, TimelineOps::Edge edge, int delta) const
{
    int track = -1;
    const Timeline& tl = m_project->timeline();
    const SubtitleCue* c = Subtitles::find(tl, id, &track);
    if (!c) return 0;
    int lo, hi;
    neighbours(tl.subtitles[track], id, &lo, &hi);
    if (edge == TimelineOps::Edge::Start) return std::clamp(delta, lo - c->start, c->length() - 1);
    const long long maxGrow = hi == INT_MAX ? INT_MAX / 2 : hi - c->end;
    return std::clamp<long long>(delta, 1 - c->length(), maxGrow);
}

void Editor::trimSubtitle(int id, TimelineOps::Edge edge, int delta)
{
    int track = -1;
    const SubtitleCue* c = Subtitles::find(m_project->timeline(), id, &track);
    if (!c || isSubtitleTrackLocked(track)) return;
    delta = clampSubtitleTrim(id, edge, delta);
    if (delta == 0) return;
    m_project->edit(T("Untertitel trimmen"), [&](Timeline& tl) {
        if (SubtitleCue* cue = Subtitles::find(tl, id)) (edge == TimelineOps::Edge::Start ? cue->start : cue->end) += delta;
    });
}

int Editor::importSubtitles(const QVector<SubtitleCue>& cues, const QString& name)
{
    if (cues.isEmpty()) return -1;
    const Timeline& cur = m_project->timeline();
    const int index = cur.subtitles.size();
    const bool anyOn = std::any_of(cur.subtitles.begin(), cur.subtitles.end(), [](const SubtitleTrack& t) { return t.enabled; });
    Project* p = m_project;
    m_project->edit(T("Untertitel importieren"), [&](Timeline& tl) {
        SubtitleTrack t;
        t.name = name.simplified();
        t.style = Subtitles::defaultStyle(p->format().size());
        t.enabled = !anyOn;
        for (SubtitleCue c : cues) {
            c.id = p->newClipId();
            Subtitles::place(t, c);
        }
        tl.subtitles << t;
    });
    return index;
}
