// SPDX-License-Identifier: AGPL-3.0-only
// Final Cut Pro 7 XML (xmeml): Premiere Pro (File › Export › Final Cut Pro XML) and DaVinci Resolve (Timeline ›
// Export › FCP 7 XML) write and read it. Conventions of the format: a clip next to a transition has start or end
// -1 (its edit point follows from the transition and its alignment); in/out are frames of the (retimed) media;
// picture and sound of one clip reference each other with <link>.
#include "core/Interchange.h"

#include "core/I18n.h"
#include "core/Timecode.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cmath>
#include <functional>

namespace Interchange {

namespace {

// ---------- Writing ----------

void writeRate(QXmlStreamWriter& x, FrameRate r)
{
    x.writeStartElement("rate");
    x.writeTextElement("timebase", QString::number(r.timebase()));
    x.writeTextElement("ntsc", r.isInteger() ? "FALSE" : "TRUE");
    x.writeEndElement();
}

void writeParameter(QXmlStreamWriter& x, const QString& id, const QString& name, double lo, double hi, double value,
                    const QVector<Key>& keys, const std::function<double(double)>& convert)
{
    x.writeStartElement("parameter");
    x.writeTextElement("parameterid", id);
    x.writeTextElement("name", name);
    x.writeTextElement("valuemin", QString::number(lo));
    x.writeTextElement("valuemax", QString::number(hi));
    x.writeTextElement("value", QString::number(convert(value), 'g', 6));
    for (const Key& k : keys) {
        x.writeStartElement("keyframe");
        x.writeTextElement("when", QString::number(k.frame));
        x.writeTextElement("value", QString::number(convert(k.value), 'g', 6));
        x.writeEndElement();
    }
    x.writeEndElement();
}

void startEffect(QXmlStreamWriter& x, const QString& name, const QString& id, const QString& category,
                 const QString& type, const QString& mediaType)
{
    x.writeStartElement("effect");
    x.writeTextElement("name", name);
    x.writeTextElement("effectid", id);
    x.writeTextElement("effectcategory", category);
    x.writeTextElement("effecttype", type);
    x.writeTextElement("mediatype", mediaType);
}

double dbToGain(double db) { return db <= kMinVolumeDb ? 0.0 : std::pow(10.0, db / 20.0); }
double gainToDb(double g) { return g <= 1e-5 ? kMinVolumeDb : 20.0 * std::log10(g); }

// ---------- Reading: a small element tree ----------

struct Node {
    QString name, text;
    QHash<QString, QString> attrs;
    QVector<Node> children;

    const Node* child(const QString& n) const
    {
        for (const Node& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
    QVector<const Node*> all(const QString& n) const
    {
        QVector<const Node*> out;
        for (const Node& c : children)
            if (c.name == n) out << &c;
        return out;
    }
    // Text of a child path "rate/timebase" (empty if missing)
    QString at(const QString& path) const
    {
        const Node* n = this;
        for (const QString& part : path.split('/')) {
            n = n->child(part);
            if (!n) return {};
        }
        return n->text.trimmed();
    }
    // Number of a child path, rounded ("1055.0" in files of Nuke Studio)
    int num(const QString& path, int fallback = 0) const
    {
        bool ok = false;
        const double v = at(path).toDouble(&ok);
        return ok ? int(std::lround(v)) : fallback;
    }
};

Node parseTree(const QByteArray& data)
{
    QXmlStreamReader r(data);
    Node root;
    QVector<Node*> stack{&root};
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) {
            Node n;
            n.name = r.name().toString();
            for (const QXmlStreamAttribute& a : r.attributes()) n.attrs.insert(a.name().toString(), a.value().toString());
            stack.last()->children << n;
            stack << &stack.last()->children.last();
        } else if (r.isEndElement()) {
            stack.removeLast();
        } else if (r.isCharacters() && !r.isWhitespace()) {
            stack.last()->text += r.text();
        }
    }
    if (r.hasError()) throw Error{T("Kein lesbares XML (Zeile %1): %2").arg(r.lineNumber()).arg(r.errorString())};
    return root;
}

const Node* findFirst(const Node& n, const QString& name)
{
    for (const Node& c : n.children) {
        if (c.name == name) return &c;
        if (const Node* f = findFirst(c, name)) return f;
    }
    return nullptr;
}

void collectFiles(const Node& n, QHash<QString, const Node*>& files)
{
    for (const Node& c : n.children) {
        if (c.name == "file" && c.attrs.contains("id") && (c.child("pathurl") || c.child("name"))) files.insert(c.attrs["id"], &c);
        collectFiles(c, files);
    }
}

FrameRate rateOf(const Node& n, FrameRate fallback)
{
    const Node* r = n.child("rate");
    if (!r) return fallback;
    const int tb = r->num("timebase");
    if (tb <= 0 || tb > 1000) return fallback; // more than 1000 frames per second: no program's, tb * 1000 overflowed
    if (r->at("ntsc").toUpper() == "TRUE") return FrameRate{tb * 1000, 1001};
    return FrameRate{tb, 1};
}

} // namespace

QByteArray writeFcp7(Sequence& seq)
{
    QByteArray out;
    QXmlStreamWriter x(&out);
    x.setAutoFormatting(true);
    x.setAutoFormattingIndent(2);
    x.writeStartDocument();
    x.writeDTD("<!DOCTYPE xmeml>");
    x.writeStartElement("xmeml");
    x.writeAttribute("version", "4");
    x.writeStartElement("sequence");
    x.writeAttribute("id", "sequence-1");
    x.writeTextElement("name", seq.name);
    int duration = 0;
    for (const QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (const TrackData& td : *tracks)
            for (const Item& it : td.items) duration = std::max(duration, it.start + it.length);
    x.writeTextElement("duration", QString::number(duration));
    writeRate(x, seq.format.rate);
    x.writeStartElement("timecode");
    writeRate(x, seq.format.rate);
    x.writeTextElement("string", Timecode::format(0, seq.format.rate.timebase()));
    x.writeTextElement("frame", "0");
    x.writeTextElement("displayformat", "NDF");
    x.writeEndElement();

    // Ids of all clip items first (links refer to items on other tracks)
    struct Ref {
        QString id;
        bool video;
        int track, index; // 1-based in the file
    };
    QHash<int, QVector<Ref>> linked; // link -> items
    int nextId = 0;
    QHash<const Item*, QString> idOf;
    for (const bool video : {true, false}) {
        const QVector<TrackData>& tracks = video ? seq.video : seq.audio;
        for (int t = 0; t < tracks.size(); ++t)
            for (int i = 0; i < tracks[t].items.size(); ++i) {
                const Item& it = tracks[t].items[i];
                const QString id = QString("clipitem-%1").arg(++nextId);
                idOf.insert(&it, id);
                if (it.link > 0) linked[it.link] << Ref{id, video, t + 1, i + 1};
            }
    }
    QHash<QString, QString> fileIds; // path -> id (the first use writes the description)

    x.writeStartElement("media");
    for (const bool video : {true, false}) {
        const QVector<TrackData>& tracks = video ? seq.video : seq.audio;
        x.writeStartElement(video ? "video" : "audio");
        if (video) {
            x.writeStartElement("format");
            x.writeStartElement("samplecharacteristics");
            writeRate(x, seq.format.rate);
            x.writeTextElement("width", QString::number(seq.format.width));
            x.writeTextElement("height", QString::number(seq.format.height));
            x.writeTextElement("pixelaspectratio", "square");
            x.writeEndElement();
            x.writeEndElement();
        }
        for (const TrackData& td : tracks) {
            x.writeStartElement("track");
            x.writeTextElement("enabled", td.enabled ? "TRUE" : "FALSE");
            // Transitions at item edges; a dissolve is written between its two items
            QHash<int, const Transition*> inOf, outOf;
            for (const Transition& tr : td.transitions) {
                if (tr.right >= 0) inOf.insert(tr.right, &tr);
                if (tr.left >= 0) outOf.insert(tr.left, &tr);
            }
            const auto writeTransition = [&](const Transition& tr, const Item* left) {
                x.writeStartElement("transitionitem");
                x.writeTextElement("start", QString::number(tr.start));
                x.writeTextElement("end", QString::number(tr.end));
                QString align = "center";
                if (tr.left < 0) align = "start-black";
                else if (tr.right < 0) align = "end-black";
                else if (left) {
                    const int cut = left->start + left->length;
                    align = cut == tr.start ? "start" : cut == tr.end ? "end" : "center";
                }
                x.writeTextElement("alignment", align);
                writeRate(x, seq.format.rate);
                const bool dip = tr.type == TransitionType::DipToColor;
                if (video) startEffect(x, dip ? "Dip to Color Dissolve" : "Cross Dissolve", dip ? "Dip To Color Dissolve" : "Cross Dissolve",
                                       "Dissolve", "transition", "video");
                else startEffect(x, "Cross Fade (+3dB)", "KGAudioTransCrossFade3dB", "", "transition", "audio");
                if (video && dip) {
                    x.writeStartElement("parameter");
                    x.writeTextElement("parameterid", "color");
                    x.writeTextElement("name", "Color");
                    x.writeStartElement("value");
                    x.writeTextElement("alpha", "0");
                    x.writeTextElement("red", QString::number(tr.color.red()));
                    x.writeTextElement("green", QString::number(tr.color.green()));
                    x.writeTextElement("blue", QString::number(tr.color.blue()));
                    x.writeEndElement();
                    x.writeEndElement();
                }
                x.writeEndElement(); // effect
                x.writeEndElement(); // transitionitem
            };
            for (int i = 0; i < td.items.size(); ++i) {
                const Item& it = td.items[i];
                const Transition* in = inOf.value(i);
                if (in && in->left < 0) writeTransition(*in, nullptr); // fade in
                const Transition* out = outOf.value(i);
                x.writeStartElement("clipitem");
                x.writeAttribute("id", idOf.value(&it));
                x.writeTextElement("name", it.name.isEmpty() ? QFileInfo(it.path).fileName() : it.name);
                x.writeTextElement("enabled", it.enabled ? "TRUE" : "FALSE");
                const int mediaFrames = it.mediaLength > 0 ? int(std::lround(it.mediaLength / it.speed)) : it.in + it.length;
                x.writeTextElement("duration", QString::number(mediaFrames));
                writeRate(x, seq.format.rate);
                // Next to a transition: start/end -1 (the transition's edit point), in/out include the part under it
                x.writeTextElement("start", in ? "-1" : QString::number(it.start));
                x.writeTextElement("end", out ? "-1" : QString::number(it.start + it.length));
                x.writeTextElement("in", QString::number(it.in - (in ? it.start - in->start : 0)));
                x.writeTextElement("out", QString::number(it.in + (out ? out->end - it.start : it.length)));
                // The file: described once, later uses refer to its id
                x.writeStartElement("file");
                if (fileIds.contains(it.path)) {
                    x.writeAttribute("id", fileIds.value(it.path));
                } else {
                    const QString fid = QString("file-%1").arg(fileIds.size() + 1);
                    fileIds.insert(it.path, fid);
                    x.writeAttribute("id", fid);
                    x.writeTextElement("name", QFileInfo(it.path).fileName());
                    // like Final Cut and Premiere: file://localhost/…
                    x.writeTextElement("pathurl", urlFromPath(it.path).replace(QRegularExpression("^file:///"), "file://localhost/"));
                    writeRate(x, seq.format.rate);
                    if (it.mediaLength > 0) x.writeTextElement("duration", QString::number(it.mediaLength));
                    x.writeStartElement("timecode");
                    writeRate(x, seq.format.rate);
                    x.writeTextElement("string", Timecode::format(it.mediaStart, seq.format.rate.timebase()));
                    x.writeTextElement("frame", QString::number(it.mediaStart));
                    x.writeTextElement("displayformat", "NDF");
                    x.writeEndElement();
                    x.writeStartElement("media");
                    bool hasVideo = false, hasAudio = false;
                    for (const TrackData& v : seq.video)
                        for (const Item& o : v.items) hasVideo = hasVideo || o.path == it.path;
                    for (const TrackData& a : seq.audio)
                        for (const Item& o : a.items) hasAudio = hasAudio || o.path == it.path;
                    if (hasVideo) {
                        x.writeStartElement("video");
                        x.writeStartElement("samplecharacteristics");
                        writeRate(x, seq.format.rate);
                        x.writeEndElement();
                        x.writeEndElement();
                    }
                    if (hasAudio) {
                        x.writeStartElement("audio");
                        x.writeTextElement("channelcount", "2");
                        x.writeEndElement();
                    }
                    x.writeEndElement(); // media
                }
                x.writeEndElement(); // file
                if (!video) {
                    x.writeStartElement("sourcetrack");
                    x.writeTextElement("mediatype", "audio");
                    x.writeTextElement("trackindex", QString::number(qint64(it.audioStream) + 1)); // any number from a file
                    x.writeEndElement();
                }
                if (it.speed != 1.0 || it.reverse) {
                    x.writeStartElement("filter");
                    startEffect(x, "Time Remap", "timeremap", "motion", "motion", "video");
                    writeParameter(x, "speed", "speed", -100000, 100000, it.speed * 100, {}, [](double v) { return v; });
                    x.writeStartElement("parameter");
                    x.writeTextElement("parameterid", "reverse");
                    x.writeTextElement("name", "reverse");
                    x.writeTextElement("value", it.reverse ? "TRUE" : "FALSE");
                    x.writeEndElement();
                    x.writeEndElement(); // effect
                    x.writeEndElement(); // filter
                }
                if (video && (it.opacity != 100 || !it.opacityKeys.isEmpty())) {
                    x.writeStartElement("filter");
                    startEffect(x, "Opacity", "opacity", "motion", "motion", "video");
                    writeParameter(x, "opacity", "Opacity", 0, 100, it.opacity, it.opacityKeys, [](double v) { return v; });
                    x.writeEndElement();
                    x.writeEndElement();
                }
                if (!video && (it.volumeDb != 0 || !it.volumeKeys.isEmpty())) {
                    x.writeStartElement("filter");
                    startEffect(x, "Audio Levels", "audiolevels", "audiolevels", "audiolevels", "audio");
                    writeParameter(x, "level", "Level", 0, 3.98109, it.volumeDb, it.volumeKeys, dbToGain);
                    x.writeEndElement();
                    x.writeEndElement();
                }
                if (it.link > 0)
                    for (const Ref& r : linked.value(it.link)) {
                        x.writeStartElement("link");
                        x.writeTextElement("linkclipref", r.id);
                        x.writeTextElement("mediatype", r.video ? "video" : "audio");
                        x.writeTextElement("trackindex", QString::number(r.track));
                        x.writeTextElement("clipindex", QString::number(r.index));
                        x.writeEndElement();
                    }
                x.writeEndElement(); // clipitem
                if (out) writeTransition(*out, &it); // dissolve to the next item or fade out
            }
            x.writeEndElement(); // track
        }
        x.writeEndElement(); // video/audio
    }
    x.writeEndElement(); // media
    for (int m : seq.markers) {
        x.writeStartElement("marker");
        x.writeTextElement("name", "");
        x.writeTextElement("comment", "");
        x.writeTextElement("in", QString::number(m));
        x.writeTextElement("out", "-1");
        x.writeEndElement();
    }
    x.writeEndElement(); // sequence
    x.writeEndElement(); // xmeml
    x.writeEndDocument();
    return out;
}

Sequence readFcp7(const QByteArray& data)
{
    const Node root = parseTree(data);
    const Node* xmeml = findFirst(root, "xmeml");
    if (!xmeml) throw Error{T("Keine Final-Cut-Pro-7-XML-Datei (kein <xmeml>)")};
    const Node* sq = findFirst(*xmeml, "sequence");
    if (!sq) throw Error{T("Die XML-Datei enthält keine Sequenz")};
    Sequence seq;
    int sequences = 0;
    std::function<void(const Node&)> count = [&](const Node& n) {
        for (const Node& c : n.children) {
            if (c.name == "sequence") ++sequences;
            else count(c);
        }
    };
    count(*xmeml);
    if (sequences > 1) seq.warnings << T("Die Datei enthält %1 Sequenzen, die erste (%2) wurde importiert").arg(sequences).arg(sq->at("name"));
    seq.name = sq->at("name");
    seq.format.rate = rateOf(*sq, FrameRate{25, 1});
    if (const Node* sc = findFirst(*sq, "samplecharacteristics"); sc && sc->num("width") > 0 && sc->num("height") > 0) {
        seq.format.width = sc->num("width");
        seq.format.height = sc->num("height");
        seq.hasSize = true;
    }
    seq.format = withinLimits(seq.format); // an empty project takes it
    QHash<QString, const Node*> files;
    collectFiles(root, files);
    for (const Node* m : sq->all("marker")) seq.markers << m->num("in");

    // Links: union of linkclipref ids -> one link number per group
    QHash<QString, int> linkOf;
    int nextLink = 0;
    QSet<QString> effectsLost, generators, nested;
    bool otherTransitions = false;
    const Node* media = sq->child("media");
    if (!media) throw Error{T("Die Sequenz enthält kein <media>")};
    for (const bool video : {true, false}) {
        const Node* kind = media->child(video ? "video" : "audio");
        if (!kind) continue;
        for (const Node* tn : kind->all("track")) {
            TrackData td;
            td.enabled = tn->at("enabled").toUpper() != "FALSE";
            // Clip items and transitions in file order. A clip item next to a transition has start or end -1: it
            // begins or ends at the transition's edit point, its in/out also cover the part under the transition.
            QVector<const Node*> els;
            for (const Node& el : tn->children)
                if (el.name == "clipitem" || el.name == "transitionitem" || el.name == "generatoritem") els << &el;
            const auto cutOf = [](const Node& tr) {
                const int start = tr.num("start"), end = tr.num("end");
                const QString align = tr.at("alignment").toLower();
                return align.startsWith("start") ? start : align.startsWith("end") ? end : (start + end) / 2;
            };
            QVector<int> itemOf(els.size(), -1); // element -> index in td.items
            for (int e = 0; e < els.size(); ++e) {
                const Node& el = *els[e];
                if (el.name == "generatoritem") {
                    generators << el.at("name");
                    continue;
                }
                if (el.name != "clipitem") continue;
                Item it;
                it.name = el.at("name");
                if (el.child("sequence")) { // a nested sequence
                    nested << it.name;
                    continue;
                }
                const Node* f = el.child("file");
                const Node* def = f ? files.value(f->attrs.value("id"), f) : nullptr;
                const QString url = def ? def->at("pathurl") : QString();
                if (url.isEmpty()) { // Premiere's generators (bars, black video, titles) are clip items without a file
                    generators << it.name;
                    continue;
                }
                it.path = pathFromUrl(url);
                if (const int d = def->num("duration"); d > 0)
                    it.mediaLength = int(std::lround(d * seq.format.rate.fps() / rateOf(*def, seq.format.rate).fps()));
                it.enabled = el.at("enabled").toUpper() != "FALSE";
                // in/out count in the clip item's own rate (Premiere: the file's), start/end in the sequence's
                const double k = seq.format.rate.fps() / rateOf(el, seq.format.rate).fps();
                const int in = int(std::lround(el.num("in") * k)), out = int(std::lround(el.num("out") * k));
                const Node* before = e > 0 && els[e - 1]->name == "transitionitem" ? els[e - 1] : nullptr;
                const Node* after = e + 1 < els.size() && els[e + 1]->name == "transitionitem" ? els[e + 1] : nullptr;
                int start = el.num("start", -1), end = el.num("end", -1);
                it.in = in;
                if (start < 0 && before) {
                    start = cutOf(*before);
                    it.in += start - before->num("start");
                }
                if (end < 0 && after) end = cutOf(*after);
                if (start < 0) start = end >= 0 ? end - (out - in) : 0;
                if (end < 0) end = start + (out - in);
                // Nothing before the timeline's start (a clip ending at a transition close to it) or before the
                // file's (a transition that ends before it starts): that part is cut off
                const int cut = std::max({0, -start, -it.in});
                start += cut;
                it.in += cut;
                it.start = start;
                it.length = std::max(0, end - start);
                if (it.name.isEmpty()) it.name = QFileInfo(it.path).fileName();
                if (!video) it.audioStream = std::max(1, el.num("sourcetrack/trackindex", 1)) - 1;
                for (const Node* filter : el.all("filter")) {
                    const Node* eff = filter->child("effect");
                    if (!eff) continue;
                    const QString id = eff->at("effectid").toLower();
                    const auto param = [eff](const QString& pid) -> const Node* {
                        for (const Node* p : eff->all("parameter"))
                            if (p->at("parameterid").toLower() == pid) return p;
                        return nullptr;
                    };
                    const auto keys = [k](const Node* p, const std::function<double(double)>& convert) {
                        QVector<Key> out;
                        for (const Node* key : p->all("keyframe"))
                            out << Key{int(std::lround(key->num("when") * k)), convert(key->at("value").toDouble())};
                        return out;
                    };
                    if (id == "timeremap") {
                        if (const Node* p = param("speed")) {
                            const double v = p->at("value").toDouble() / 100.0;
                            if (v != 0) {
                                it.speed = std::abs(v);
                                it.reverse = v < 0;
                            }
                        }
                        if (const Node* p = param("reverse")) it.reverse = it.reverse || p->at("value").toUpper() == "TRUE";
                        if (const Node* p = param("variablespeed"); p && p->at("value") == "1") effectsLost << T("variable Geschwindigkeit");
                    } else if (id == "audiolevels") {
                        if (const Node* p = param("level")) {
                            it.volumeDb = gainToDb(p->at("value").isEmpty() ? 1.0 : p->at("value").toDouble());
                            it.volumeKeys = keys(p, gainToDb);
                        }
                    } else if (id == "opacity") {
                        if (const Node* p = param("opacity")) {
                            it.opacity = p->at("value").isEmpty() ? 100 : p->at("value").toDouble();
                            it.opacityKeys = keys(p, [](double v) { return v; });
                        }
                    } else if (id != "basic" && id != "audiopan" && !eff->at("name").isEmpty()) {
                        effectsLost << eff->at("name");
                    }
                }
                // Link group
                QStringList refs{el.attrs.value("id")};
                for (const Node* l : el.all("link")) refs << l->at("linkclipref");
                refs.removeAll(QString());
                int link = 0; // links may be written on one side only
                for (const QString& r : refs) link = std::max(link, linkOf.value(r));
                if (!link && refs.size() > 1) link = ++nextLink;
                if (link)
                    for (const QString& r : refs) linkOf.insert(r, link);
                it.link = link;
                itemOf[e] = int(td.items.size());
                td.items << it;
            }
            // Transitions: a dissolve between the items before and after it, a fade from/to black on one side
            for (int e = 0; e < els.size(); ++e) {
                const Node& el = *els[e];
                if (el.name != "transitionitem") continue;
                Transition tr;
                tr.start = el.num("start");
                tr.end = el.num("end");
                const QString align = el.at("alignment").toLower();
                if (align != "start-black") tr.left = e > 0 ? itemOf[e - 1] : -1;
                if (align != "end-black") tr.right = e + 1 < els.size() ? itemOf[e + 1] : -1;
                const Node* eff = el.child("effect");
                const QString name = eff ? eff->at("name") + " " + eff->at("effectid") : QString();
                tr.type = name.contains("Dip", Qt::CaseInsensitive) ? TransitionType::DipToColor : TransitionType::CrossDissolve;
                if (!name.contains("Dissolve", Qt::CaseInsensitive) && !name.contains("Fade", Qt::CaseInsensitive)
                    && !name.contains("Dip", Qt::CaseInsensitive))
                    otherTransitions = true;
                if (eff)
                    for (const Node* p : eff->all("parameter"))
                        if (p->at("parameterid") == "color")
                            if (const Node* v = p->child("value")) tr.color = QColor(v->num("red"), v->num("green"), v->num("blue"));
                if (tr.left >= 0 || tr.right >= 0) td.transitions << tr;
            }
            (video ? seq.video : seq.audio) << td;
        }
    }
    // An item linked to nothing else in the end is not linked
    QHash<int, int> uses;
    for (const QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (const TrackData& td : *tracks)
            for (const Item& it : td.items) uses[it.link] += it.link > 0;
    for (QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (TrackData& td : *tracks)
            for (Item& it : td.items)
                if (uses.value(it.link) < 2) it.link = 0;
    if (!effectsLost.isEmpty())
        seq.warnings << T("Effekte nicht übernommen: %1").arg(QStringList(effectsLost.begin(), effectsLost.end()).join(", "));
    if (!generators.isEmpty())
        seq.warnings << T("Generatoren/Titel weggelassen: %1").arg(QStringList(generators.begin(), generators.end()).join(", "));
    if (!nested.isEmpty())
        seq.warnings << T("Verschachtelte Sequenzen weggelassen: %1").arg(QStringList(nested.begin(), nested.end()).join(", "));
    if (otherTransitions) seq.warnings << T("Andere Übergänge als Blenden als Überblendung gelesen");
    return seq;
}

} // namespace Interchange
