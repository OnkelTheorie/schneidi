// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TimelineStory.h"

#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace TimelineStory {

namespace {

constexpr const char* kKinds[] = {"hook", "build", "question", "turn", "resolution", "cta", "free"};

int intOf(const QJsonValue& v, int fallback = 0)
{
    return v.isDouble() ? int(std::clamp(v.toDouble(), -1e9, 1e9)) : fallback;
}

} // namespace

QString kindName(StoryKind k)
{
    return QString::fromLatin1(kKinds[int(k)]);
}

std::optional<StoryKind> kindFromName(const QString& name)
{
    for (int i = 0; i < int(std::size(kKinds)); ++i)
        if (name.compare(QLatin1String(kKinds[i]), Qt::CaseInsensitive) == 0) return StoryKind(i);
    return std::nullopt;
}

QStringList kindNames()
{
    QStringList out;
    for (const char* k : kKinds) out << QString::fromLatin1(k);
    return out;
}

QJsonArray sectionsToJson(const QVector<StorySection>& sections)
{
    QJsonArray a;
    for (const StorySection& s : sections) {
        QJsonObject o{{"kind", kindName(s.kind)}, {"from", s.from}, {"to", s.to}};
        if (!s.name.isEmpty()) o["name"] = s.name;
        a << o;
    }
    return a;
}

QVector<StorySection> sectionsFromJson(const QJsonArray& a)
{
    QVector<StorySection> out;
    for (const QJsonValue& v : a) {
        const QJsonObject o = v.toObject();
        StorySection s;
        s.kind = kindFromName(o.value("kind").toString()).value_or(StoryKind::Free);
        s.name = o.value("name").toString();
        s.from = std::max(0, intOf(o.value("from")));
        s.to = intOf(o.value("to"));
        if (s.to <= s.from) continue;
        out << s;
    }
    std::stable_sort(out.begin(), out.end(), [](const StorySection& x, const StorySection& y) { return x.from < y.from; });
    return out;
}

QJsonArray notesToJson(const QVector<TimelineNote>& notes)
{
    QJsonArray a;
    for (const TimelineNote& n : notes) {
        QJsonObject o{{"id", n.id}, {"at", n.at}, {"text", n.text}};
        if (n.done) o["done"] = true;
        a << o;
    }
    return a;
}

QVector<TimelineNote> notesFromJson(const QJsonArray& a)
{
    QVector<TimelineNote> out;
    QSet<int> used;
    int highest = 0;
    for (const QJsonValue& v : a) {
        const QJsonObject o = v.toObject();
        TimelineNote n{intOf(o.value("id")), std::max(0, intOf(o.value("at"))), o.value("text").toString(),
                       o.value("done").toBool()};
        if (n.id <= 0 || used.contains(n.id)) n.id = 0; // a new one below
        else used.insert(n.id);
        highest = std::max(highest, n.id);
        out << n;
    }
    for (TimelineNote& n : out)
        if (n.id == 0) n.id = ++highest;
    std::stable_sort(out.begin(), out.end(), [](const TimelineNote& x, const TimelineNote& y) { return x.at < y.at; });
    return out;
}

void retime(QVector<StorySection>& sections, QVector<TimelineNote>& notes, double k)
{
    const auto at = [k](int frame) { return std::max(0, int(std::lround(frame * k))); };
    for (StorySection& s : sections) {
        s.from = at(s.from);
        s.to = std::max(s.from + 1, at(s.to));
    }
    for (TimelineNote& n : notes) n.at = at(n.at);
}

} // namespace TimelineStory
