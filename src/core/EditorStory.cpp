// SPDX-License-Identifier: AGPL-3.0-only
// Editor: story sections, notes and teasers of the open timeline (0.17 package "story", core/Story.h)
#include "core/Editor.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "core/Story.h"

#include <algorithm>

QString Editor::addSection(const StorySection& section)
{
    QVector<StorySection> sections = m_project->timeline().sections;
    if (const QString error = Story::add(sections, section); !error.isEmpty()) return error;
    m_project->edit(T("Abschnitt anlegen"), [&](Timeline& tl) { tl.sections = sections; });
    return {};
}

bool Editor::removeSectionAt(int frame)
{
    const int i = Story::at(m_project->timeline().sections, frame);
    if (i < 0) return false;
    m_project->edit(T("Abschnitt entfernen"), [&](Timeline& tl) { tl.sections.remove(i); });
    return true;
}

void Editor::setSections(const QVector<StorySection>& sections, const QString& text)
{
    QVector<StorySection> sorted = sections;
    std::stable_sort(sorted.begin(), sorted.end(), [](const StorySection& a, const StorySection& b) { return a.from < b.from; });
    if (sorted == m_project->timeline().sections) return;
    m_project->edit(text, [&](Timeline& tl) { tl.sections = sorted; });
}

bool Editor::setSectionRange(int index, int from, int to, const QString& mergeKey)
{
    QVector<StorySection> sections = m_project->timeline().sections;
    if (index < 0 || index >= sections.size()) return false;
    StorySection s = sections.takeAt(index);
    if (s.from == from && s.to == to) return true;
    s.from = from;
    s.to = to;
    if (!Story::add(sections, s).isEmpty()) return false;
    m_project->edit(T("Abschnitt ändern"), [&](Timeline& tl) { tl.sections = sections; }, mergeKey);
    return true;
}

int Editor::addNote(int frame, const QString& text)
{
    int id = 0;
    for (const TimelineNote& n : m_project->timeline().notes) id = std::max(id, n.id);
    ++id;
    m_project->edit(T("Notiz anlegen"), [&](Timeline& tl) {
        const TimelineNote n{id, std::max(0, frame), text, false};
        const auto pos = std::upper_bound(tl.notes.begin(), tl.notes.end(), n,
                                          [](const TimelineNote& a, const TimelineNote& b) { return a.at < b.at; });
        tl.notes.insert(pos, n);
    });
    return id;
}

bool Editor::setNoteDone(int id, bool done)
{
    const QVector<TimelineNote>& notes = m_project->timeline().notes;
    const auto it = std::find_if(notes.begin(), notes.end(), [&](const TimelineNote& n) { return n.id == id; });
    if (it == notes.end()) return false;
    if (it->done == done) return true;
    m_project->edit(done ? T("Notiz erledigt") : T("Notiz wieder offen"), [&](Timeline& tl) {
        for (TimelineNote& n : tl.notes)
            if (n.id == id) n.done = done;
    });
    return true;
}

bool Editor::removeNote(int id)
{
    const QVector<TimelineNote>& notes = m_project->timeline().notes;
    if (std::none_of(notes.begin(), notes.end(), [&](const TimelineNote& n) { return n.id == id; })) return false;
    m_project->edit(T("Notiz entfernen"), [&](Timeline& tl) {
        tl.notes.erase(std::remove_if(tl.notes.begin(), tl.notes.end(), [&](const TimelineNote& n) { return n.id == id; }),
                       tl.notes.end());
    });
    return true;
}

bool Editor::prependTeaser(int from, int to)
{
    Timeline copy = m_project->timeline();
    Project* p = m_project;
    if (!Story::prependTeaser(copy, from, to, [p] { return p->newClipId(); }, [p] { return p->newLinkId(); }))
        return false;
    m_project->edit(T("Teaser voranstellen"), [&](Timeline& tl) { tl = copy; });
    return true;
}
