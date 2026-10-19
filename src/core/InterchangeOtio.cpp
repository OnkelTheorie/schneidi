// OpenTimelineIO (.otio, JSON): DaVinci Resolve (18.5+), Premiere (via adapters), Avid, Nuke Studio and others.
// Tracks hold clips and gaps back to back; a transition sits between two of them and overlaps both (in_offset
// before the cut, out_offset after it). Volume, opacity, the audio stream and links have no standard field: they go
// into the clip's metadata under "schneidi" (other programs ignore them).
#include "core/Interchange.h"

#include "core/I18n.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>

namespace Interchange {

namespace {

QJsonObject rationalTime(double frames, double rate)
{
    return {{"OTIO_SCHEMA", "RationalTime.1"}, {"rate", rate}, {"value", frames}};
}

QJsonObject timeRange(double start, double duration, double rate)
{
    return {{"OTIO_SCHEMA", "TimeRange.1"}, {"start_time", rationalTime(start, rate)}, {"duration", rationalTime(duration, rate)}};
}

QJsonObject gap(int frames, double rate)
{
    return {{"OTIO_SCHEMA", "Gap.1"}, {"name", ""}, {"source_range", timeRange(0, frames, rate)}, {"effects", QJsonArray{}},
            {"markers", QJsonArray{}}, {"metadata", QJsonObject{}}, {"enabled", true}};
}

QJsonObject transition(int before, int after, double rate, const Transition& tr)
{
    QJsonObject meta;
    if (tr.type == TransitionType::DipToColor) meta["schneidi"] = QJsonObject{{"type", "dip"}, {"color", tr.color.name()}};
    return {{"OTIO_SCHEMA", "Transition.1"}, {"name", tr.type == TransitionType::DipToColor ? "Dip to Color" : "Cross Dissolve"},
            {"transition_type", "SMPTE_Dissolve"}, {"in_offset", rationalTime(before, rate)},
            {"out_offset", rationalTime(after, rate)}, {"metadata", meta}};
}

// Frames of a RationalTime in the sequence's rate
int frames(const QJsonValue& rt, double rate)
{
    const QJsonObject o = rt.toObject();
    const double r = o["rate"].toDouble(rate);
    return int(std::lround(o["value"].toDouble() * (r > 0 ? rate / r : 1.0)));
}

} // namespace

QByteArray writeOtio(Sequence& seq)
{
    const double rate = seq.format.rate.fps();
    QJsonArray stackChildren;
    for (const bool video : {true, false}) {
        const QVector<TrackData>& tracks = video ? seq.video : seq.audio;
        for (int t = 0; t < tracks.size(); ++t) {
            const TrackData& td = tracks[t];
            QHash<int, const Transition*> inOf, outOf;
            for (const Transition& tr : td.transitions) {
                if (tr.right >= 0) inOf.insert(tr.right, &tr);
                if (tr.left >= 0 && tr.right < 0) outOf.insert(tr.left, &tr);
            }
            QJsonArray children;
            int pos = 0;
            for (int i = 0; i < td.items.size(); ++i) {
                const Item& it = td.items[i];
                const Transition* in = inOf.value(i);
                if (it.start > pos || (in && in->left < 0)) {
                    children << gap(std::max(0, it.start - pos), rate); // a fade in needs something to fade from
                    pos = it.start;
                }
                if (in) children << transition(it.start - in->start, in->end - it.start, rate, *in);
                QJsonObject meta{{"volume_db", it.volumeDb}, {"opacity", it.opacity}, {"audio_stream", it.audioStream}};
                if (it.link > 0) meta["link"] = it.link;
                if (!it.volumeKeys.isEmpty() || !it.opacityKeys.isEmpty()) {
                    QJsonArray keys;
                    for (const Key& k : video ? it.opacityKeys : it.volumeKeys) keys << QJsonArray{k.frame, k.value};
                    meta[video ? "opacity_keys" : "volume_keys"] = keys;
                }
                const double fileStart = it.in * it.speed;
                QJsonObject ref{{"OTIO_SCHEMA", "ExternalReference.1"}, {"name", QFileInfo(it.path).fileName()},
                                {"target_url", urlFromPath(it.path)}, {"metadata", QJsonObject{}}};
                if (it.mediaLength > 0) ref["available_range"] = timeRange(it.mediaStart, it.mediaLength, rate);
                QJsonArray effects;
                if (it.speed != 1.0 || it.reverse)
                    effects << QJsonObject{{"OTIO_SCHEMA", "LinearTimeWarp.1"}, {"name", ""}, {"effect_name", "LinearTimeWarp"},
                                           {"time_scalar", it.reverse ? -it.speed : it.speed}, {"metadata", QJsonObject{}}};
                children << QJsonObject{{"OTIO_SCHEMA", "Clip.1"},
                                        {"name", it.name.isEmpty() ? QFileInfo(it.path).fileName() : it.name},
                                        {"source_range", timeRange(it.mediaStart + fileStart, it.length, rate)},
                                        {"media_reference", ref},
                                        {"effects", effects},
                                        {"markers", QJsonArray{}},
                                        {"enabled", it.enabled},
                                        {"metadata", QJsonObject{{"schneidi", meta}}}};
                pos = it.start + it.length;
                if (const Transition* out = outOf.value(i)) {
                    children << transition(pos - out->start, out->end - pos, rate, *out);
                    children << gap(0, rate); // something to fade to
                }
            }
            const QString name = td.name.isEmpty() ? QString("%1 %2").arg(video ? "Video" : "Audio").arg(t + 1) : td.name;
            stackChildren << QJsonObject{{"OTIO_SCHEMA", "Track.1"}, {"name", name}, {"kind", video ? "Video" : "Audio"},
                                         {"children", children}, {"enabled", td.enabled}, {"effects", QJsonArray{}},
                                         {"markers", QJsonArray{}}, {"metadata", QJsonObject{}}, {"source_range", QJsonValue()}};
        }
    }
    QJsonArray markers;
    for (int m : seq.markers)
        markers << QJsonObject{{"OTIO_SCHEMA", "Marker.2"}, {"name", ""}, {"color", "RED"}, {"comment", ""},
                               {"marked_range", timeRange(m, 0, rate)}, {"metadata", QJsonObject{}}};
    const QJsonObject stack{{"OTIO_SCHEMA", "Stack.1"}, {"name", "tracks"}, {"children", stackChildren},
                            {"markers", markers}, {"effects", QJsonArray{}}, {"metadata", QJsonObject{}},
                            {"enabled", true}, {"source_range", QJsonValue()}};
    const QJsonObject tl{{"OTIO_SCHEMA", "Timeline.1"},
                         {"name", seq.name},
                         {"global_start_time", rationalTime(0, rate)},
                         {"tracks", stack},
                         {"metadata", QJsonObject{{"schneidi", QJsonObject{{"width", seq.format.width},
                                                                           {"height", seq.format.height},
                                                                           {"rate_num", seq.format.rate.num},
                                                                           {"rate_den", seq.format.rate.den}}}}}};
    return QJsonDocument(tl).toJson(QJsonDocument::Indented);
}

Sequence readOtio(const QByteArray& data)
{
    // OTIO writes Inf/NaN, and other programs integers beyond 64 bits, into metadata: not valid for Qt's JSON reader,
    // read as text
    static const QRegularExpression bigInt(R"(([:\[,]\s*)(-?\d{19,}|-?Inf(?:inity)?|NaN)(?=\s*[,\]}]))");
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QString::fromUtf8(data).replace(bigInt, "\\1\"\\2\"").toUtf8(), &err);
    if (doc.isNull()) throw Error{T("Kein lesbares JSON: %1").arg(err.errorString())};
    QJsonObject tl = doc.object();
    // A collection of timelines (SerializableCollection): the first one
    if (tl["OTIO_SCHEMA"].toString().startsWith("SerializableCollection")) {
        for (const QJsonValue& c : tl["children"].toArray())
            if (c["OTIO_SCHEMA"].toString().startsWith("Timeline")) {
                tl = c.toObject();
                break;
            }
    }
    if (!tl["OTIO_SCHEMA"].toString().startsWith("Timeline")) throw Error{T("Keine OpenTimelineIO-Timeline")};
    Sequence seq;
    seq.name = std::as_const(tl)["name"].toString();
    const QJsonObject own = std::as_const(tl)["metadata"]["schneidi"].toObject();
    // Frame rate: schneidi's exact one, else the rate of the times in the file (29.97 = 30000/1001 …)
    double rate = std::as_const(tl)["global_start_time"]["rate"].toDouble();
    const QJsonArray tracks = std::as_const(tl)["tracks"]["children"].toArray();
    if (rate <= 0)
        for (const QJsonValue& t : tracks)
            for (const QJsonValue& c : t["children"].toArray())
                if (rate <= 0) rate = c["source_range"]["duration"]["rate"].toDouble();
    if (rate <= 0) rate = 25;
    seq.format.rate = own.contains("rate_num") ? FrameRate{own["rate_num"].toInt(), std::max(1, own["rate_den"].toInt())}
                                               : nearestFrameRate(rate);
    if (std::abs(seq.format.rate.fps() - rate) > 0.01) seq.format.rate = FrameRate{int(std::lround(rate)), 1};
    rate = seq.format.rate.fps();
    if (own.contains("width")) {
        seq.format.width = own["width"].toInt(1920);
        seq.format.height = own["height"].toInt(1080);
        seq.hasSize = true;
    }
    const int offset = frames(std::as_const(tl)["global_start_time"], rate);
    for (const QJsonValue& m : std::as_const(tl)["tracks"]["markers"].toArray()) seq.markers << frames(m["marked_range"]["start_time"], rate) - offset;
    QStringList skipped, generators;
    bool otherTransitions = false, effects = false;
    for (const QJsonValue& tv : tracks) {
        // "Sequence" = a track in files of OTIO before 0.9
        const QString trackSchema = tv["OTIO_SCHEMA"].toString();
        if (!trackSchema.startsWith("Track") && !trackSchema.startsWith("Sequence")) continue;
        const bool video = tv["kind"].toString() != "Audio";
        TrackData td;
        td.name = tv["name"].toString();
        // The default name the writer gives a track without one stays empty (the app shows its own default)
        if (td.name == QString("%1 %2").arg(video ? "Video" : "Audio").arg((video ? seq.video : seq.audio).size() + 1)) td.name.clear();
        td.enabled = tv["enabled"].toBool(true);
        int pos = 0;
        struct Pending {
            int before, after;
            Transition t;
            int leftItem; // -1 = a gap or nothing before
        };
        std::optional<Pending> pending;
        int lastItem = -1;      // index of the item that ended at `pos` (-1 = a gap)
        for (const QJsonValue& cv : tv["children"].toArray()) {
            const QString schema = cv["OTIO_SCHEMA"].toString();
            if (schema.startsWith("Transition")) {
                Pending p{frames(cv["in_offset"], rate), frames(cv["out_offset"], rate), {}, lastItem};
                const QJsonObject meta = cv["metadata"]["schneidi"].toObject();
                if (meta["type"] == "dip") {
                    p.t.type = TransitionType::DipToColor;
                    p.t.color = QColor(meta["color"].toString());
                } else if (!cv["transition_type"].toString().contains("Dissolve")) {
                    otherTransitions = true;
                }
                p.t.start = pos - p.before;
                p.t.end = pos + p.after;
                pending = p;
                continue;
            }
            QJsonObject ref = cv["media_reference"].toObject();
            if (ref.isEmpty()) { // Clip.2: several references
                const QJsonObject refs = cv["media_references"].toObject();
                ref = refs[cv["active_media_reference_key"].toString("DEFAULT_MEDIA")].toObject();
            }
            // Without a source range a clip plays all of its media
            const QJsonValue range = cv["source_range"].isObject() ? cv["source_range"] : ref["available_range"];
            const int length = frames(range["duration"], rate);
            const bool generator = ref["OTIO_SCHEMA"].toString().startsWith("GeneratorReference");
            if (generator) generators << cv["name"].toString(ref["generator_kind"].toString());
            if (schema.startsWith("Gap") || !schema.startsWith("Clip") || generator) {
                if (!schema.startsWith("Gap") && !generator) skipped << cv["name"].toString(schema); // nested stacks …
                if (pending) {
                    // A transition into a gap = fade out of the item before
                    if (pending->leftItem >= 0) {
                        Transition t = pending->t;
                        t.left = pending->leftItem;
                        td.transitions << t;
                    }
                    pending.reset();
                }
                pos += length;
                lastItem = -1;
                continue;
            }
            Item it;
            it.name = cv["name"].toString();
            it.start = pos;
            it.length = length;
            it.enabled = cv["enabled"].toBool(true);
            it.path = pathFromUrl(ref["target_url"].toString());
            if (it.path.isEmpty()) it.path = it.name;
            // Source times count from the media's start timecode (DaVinci: 01:00:00:00 for most camera files)
            if (ref["available_range"].isObject()) {
                it.mediaLength = frames(std::as_const(ref)["available_range"]["duration"], rate);
                it.mediaStart = std::max(0, frames(std::as_const(ref)["available_range"]["start_time"], rate));
            }
            for (const QJsonValue& e : cv["effects"].toArray()) {
                if (e["OTIO_SCHEMA"].toString().startsWith("LinearTimeWarp")) {
                    const double s = e["time_scalar"].toDouble(1.0);
                    if (s != 0) {
                        it.speed = std::abs(s);
                        it.reverse = s < 0;
                    }
                } else {
                    effects = true;
                }
            }
            const int fileStart = std::max(0, frames(range["start_time"], rate) - it.mediaStart);
            it.in = int(std::lround(fileStart / it.speed));
            const QJsonObject meta = cv["metadata"]["schneidi"].toObject();
            it.volumeDb = meta["volume_db"].toDouble(0);
            it.opacity = meta["opacity"].toDouble(100);
            it.audioStream = meta["audio_stream"].toInt(0);
            it.link = meta["link"].toInt(0);
            for (const QJsonValue& k : meta[video ? "opacity_keys" : "volume_keys"].toArray())
                (video ? it.opacityKeys : it.volumeKeys) << Key{k[0].toInt(), k[1].toDouble()};
            if (it.name.isEmpty()) it.name = QFileInfo(it.path).fileName();
            const int index = int(td.items.size());
            td.items << it;
            if (pending) {
                Transition t = pending->t;
                t.left = pending->leftItem;
                t.right = index;
                td.transitions << t;
                pending.reset();
            }
            pos += length;
            lastItem = index;
        }
        if (pending && pending->leftItem >= 0) { // fade out at the end of the track
            Transition t = pending->t;
            t.left = pending->leftItem;
            td.transitions << t;
        }
        (video ? seq.video : seq.audio) << td;
    }
    if (!skipped.isEmpty()) seq.warnings << T("Weggelassen (keine Clips): %1").arg(skipped.join(", "));
    if (!generators.isEmpty()) seq.warnings << T("Generatoren/Titel weggelassen: %1").arg(generators.join(", "));
    if (otherTransitions) seq.warnings << T("Andere Übergänge als Blenden als Überblendung gelesen");
    if (effects) seq.warnings << T("Effekte nicht übernommen");
    return seq;
}

} // namespace Interchange
