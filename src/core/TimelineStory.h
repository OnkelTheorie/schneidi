// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Story sections and timeline notes (Timeline::sections / Timeline::notes, 0.17): names of the section kinds, how
// they are stored in the project file, and how their times follow a frame rate change. Pure data functions; ops,
// checks and the user interface on them live with their packages.

#include "core/Types.h"

#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace TimelineStory {

// "hook", "build", "question", "turn", "resolution", "cta", "free"
QString kindName(StoryKind k);
std::optional<StoryKind> kindFromName(const QString& name);
QStringList kindNames();

// Project file form: sections [{kind, name?, from, to}], notes [{id, at, text, done?}]. Reading drops broken
// entries (to <= from; an unknown kind becomes free), sorts, and gives notes without an id or with a duplicate one a
// new id
QJsonArray sectionsToJson(const QVector<StorySection>& sections);
QVector<StorySection> sectionsFromJson(const QJsonArray& a);
QJsonArray notesToJson(const QVector<TimelineNote>& notes);
QVector<TimelineNote> notesFromJson(const QJsonArray& a);

// Times at a new frame rate (k = new fps / old fps): each section keeps at least one frame
void retime(QVector<StorySection>& sections, QVector<TimelineNote>& notes, double k);

} // namespace TimelineStory
