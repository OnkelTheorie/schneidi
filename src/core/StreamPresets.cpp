#include "core/StreamPresets.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <algorithm>

namespace {

constexpr const char* kSettingsKey = "import/streamPresets";

bool sameTitle(const QString& a, const QString& b)
{
    const QString x = a.trimmed();
    return !x.isEmpty() && x.compare(b.trimmed(), Qt::CaseInsensitive) == 0;
}

QString actionName(StreamRule::Action a)
{
    switch (a) {
    case StreamRule::Action::Mute: return "mute";
    case StreamRule::Action::Skip: return "skip";
    case StreamRule::Action::Place: break;
    }
    return "place";
}

QString streamTitle(const MediaInfo& m, int stream)
{
    return stream < m.audioStreams.size() ? m.audioStreams[stream].title : QString();
}

} // namespace

bool StreamPresets::isMixTitle(const QString& title)
{
    const QString t = title.trimmed();
    if (!t.startsWith("mix", Qt::CaseInsensitive)) return false;
    return t.size() == 3 || !t.at(3).isLetter();
}

QVector<StreamRule> StreamPresets::plan(const MediaInfo& m) const
{
    QVector<StreamRule> out;
    const int n = m.audioStreamCount();
    for (int k = 0; k < n; ++k) {
        StreamRule r;
        r.title = streamTitle(m, k);
        out << r;
    }
    if (n < 2) return out;

    QVector<bool> ruled(n, false);
    for (int k = 0; k < n; ++k) {
        const auto it = std::find_if(rules.begin(), rules.end(), [&](const StreamRule& r) { return sameTitle(r.title, out[k].title); });
        if (it == rules.end()) continue;
        const QString title = out[k].title;
        out[k] = *it;
        out[k].title = title;
        out[k].trackName = out[k].trackName.trimmed();
        ruled[k] = true;
    }
    if (muteMix) {
        // Only when the file has single streams besides its mixes (which are not left out)
        bool single = false;
        for (int k = 0; k < n; ++k) single = single || (!isMixTitle(out[k].title) && out[k].action != StreamRule::Action::Skip);
        if (single)
            for (int k = 0; k < n; ++k)
                if (!ruled[k] && isMixTitle(out[k].title)) out[k].action = StreamRule::Action::Mute;
    }
    if (std::all_of(out.begin(), out.end(), [](const StreamRule& r) { return r.action == StreamRule::Action::Skip; }))
        for (StreamRule& r : out) r.action = StreamRule::Action::Mute;
    return out;
}

QVector<StreamPlacement> StreamPresets::placements(const MediaInfo& m) const
{
    QVector<StreamPlacement> out;
    const QVector<StreamRule> p = plan(m);
    for (int k = 0; k < p.size(); ++k) {
        if (p[k].action == StreamRule::Action::Skip) continue;
        StreamPlacement s;
        s.stream = k;
        s.enabled = p[k].action != StreamRule::Action::Mute;
        s.volumeDb = std::clamp(p[k].volumeDb, kMinVolumeDb, kMaxVolumeDb);
        s.trackLabel = p[k].trackName.isEmpty() ? m.audioStreamTrackLabel(k) : p[k].trackName;
        out << s;
    }
    return out;
}

QVector<StreamRule> StreamPresets::learn(const Timeline& tl, const MediaInfo& m, int linkId)
{
    QVector<StreamRule> out;
    if (linkId == 0 || m.audioStreamCount() < 2) return out;
    // First clip per stream of this placement, with its track
    QVector<const Clip*> clips(m.audioStreamCount(), nullptr);
    QVector<const Track*> tracks(m.audioStreamCount(), nullptr);
    bool any = false;
    for (const Track& t : tl.audio)
        for (const Clip& c : t.clips)
            if (c.linkId == linkId && c.mediaPath == m.path && c.audioStream >= 0 && c.audioStream < clips.size()
                && !clips[c.audioStream]) {
                clips[c.audioStream] = &c;
                tracks[c.audioStream] = &t;
                any = true;
            }
    if (!any) return out;
    for (int k = 0; k < clips.size(); ++k) {
        StreamRule r;
        r.title = streamTitle(m, k).trimmed();
        if (r.title.isEmpty()) continue;
        if (!clips[k]) {
            r.action = StreamRule::Action::Skip;
        } else {
            r.action = clips[k]->enabled ? StreamRule::Action::Place : StreamRule::Action::Mute;
            r.volumeDb = clips[k]->volumeDb;
            const QString name = tracks[k]->name.trimmed();
            if (!name.isEmpty() && name != r.title) r.trackName = name;
        }
        out << r;
    }
    return out;
}

void StreamPresets::merge(const QVector<StreamRule>& learned)
{
    for (const StreamRule& r : learned) {
        auto it = std::find_if(rules.begin(), rules.end(), [&](const StreamRule& o) { return sameTitle(o.title, r.title); });
        if (it == rules.end()) {
            rules << r;
            continue;
        }
        *it = r;
        // Further rules with the same title would never apply
        rules.erase(std::remove_if(it + 1, rules.end(), [&](const StreamRule& o) { return sameTitle(o.title, r.title); }),
                    rules.end());
    }
}

QJsonObject StreamPresets::toJson() const
{
    QJsonArray list;
    for (const StreamRule& r : rules) {
        QJsonObject o{{"title", r.title}, {"action", actionName(r.action)}};
        if (r.volumeDb != 0) o["volumeDb"] = r.volumeDb;
        if (!r.trackName.isEmpty()) o["track"] = r.trackName;
        list << o;
    }
    return {{"muteMix", muteMix}, {"rules", list}};
}

StreamPresets StreamPresets::fromJson(const QJsonObject& o)
{
    StreamPresets p;
    p.muteMix = o.value("muteMix").toBool(p.muteMix);
    for (const QJsonValue& v : o.value("rules").toArray()) {
        const QJsonObject r = v.toObject();
        StreamRule rule;
        rule.title = r.value("title").toString().trimmed();
        if (rule.title.isEmpty()) continue;
        const QString a = r.value("action").toString();
        rule.action = a == "mute" ? StreamRule::Action::Mute : a == "skip" ? StreamRule::Action::Skip : StreamRule::Action::Place;
        rule.volumeDb = std::clamp(r.value("volumeDb").toDouble(), kMinVolumeDb, kMaxVolumeDb);
        rule.trackName = r.value("track").toString().trimmed();
        p.rules << rule;
    }
    return p;
}

StreamPresets StreamPresets::load()
{
    const QByteArray data = QSettings().value(kSettingsKey).toString().toUtf8();
    if (data.isEmpty()) return {};
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return fromJson(doc.object());
}

void StreamPresets::save() const
{
    if (*this == StreamPresets{}) QSettings().remove(kSettingsKey);
    else QSettings().setValue(kSettingsKey, QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact)));
}
