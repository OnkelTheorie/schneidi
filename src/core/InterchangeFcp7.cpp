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
    int num(const QString& path, int fallback = 0) const
    {
        bool ok = false;
        const int v = at(path).toInt(&ok);
        return ok ? v : fallback;
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
    if (tb <= 0) return fallback;
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
                x.writeTextElement("start", in ? "-1" : QString::number(it.start));
                x.writeTextElement("end", out ? "-1" : QString::number(it.start + it.length));
                x.writeTextElement("in", QString::number(it.in));
                x.writeTextElement("out", QString::number(it.in + it.length));
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
                    x.writeTextElement("string", Timecode::format(0, seq.format.rate.timebase()));
                    x.writeTextElement("frame", "0");
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
                    x.writeTextElement("trackindex", QString::number(it.audioStream + 1));
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
    QHash<QString, const Node*> files;
    collectFiles(root, files);
    for (const Node* m : sq->all("marker")) seq.markers << m->num("in");

    // Links: union of linkclipref ids -> one link number per group
    QHash<QString, int> linkOf;
    int nextLink = 0;
    QSet<QString> effectsLost, generators;
    bool otherTransitions = false;
    const Node* media = sq->child("media");
    if (!media) throw Error{T("Die Sequenz enthält kein <media>")};
    for (const bool video : {true, false}) {
        const Node* kind = media->child(video ? "video" : "audio");
        if (!kind) continue;
        for (const Node* tn : kind->all("track")) {
            TrackData td;
            td.enabled = tn->at("enabled").toUpper() != "FALSE";
            // In file order: clip items and transitions; a clip with start/end -1 takes its edit point from the
            // transition next to it
            struct Tr {
                int start, end, cut;
                bool fromBlack, toBlack;
                Transition t;
            };
            QVector<Tr> trs;
            QVector<int> trBefore, trAfter; // per item: index in trs or -1
            const Node* lastEl = nullptr;
            for (const Node& el : tn->children) {
                if (el.name == "transitionitem") {
                    Tr tr;
                    tr.start = el.num("start");
                    tr.end = el.num("end");
                    const QString align = el.at("alignment").toLower();
                    tr.fromBlack = align == "start-black";
                    tr.toBlack = align == "end-black";
                    tr.cut = align.startsWith("start") ? tr.start : align.startsWith("end") ? tr.end : tr.start + (tr.end - tr.start) / 2;
                    const Node* eff = el.child("effect");
                    const QString name = eff ? eff->at("name") + " " + eff->at("effectid") : QString();
                    tr.t.type = name.contains("Dip", Qt::CaseInsensitive) ? TransitionType::DipToColor : TransitionType::CrossDissolve;
                    if (!name.contains("Dissolve", Qt::CaseInsensitive) && !name.contains("Fade", Qt::CaseInsensitive)
                        && !name.contains("Dip", Qt::CaseInsensitive))
                        otherTransitions = true;
                    if (eff)
                        for (const Node* p : eff->all("parameter"))
                            if (p->at("parameterid") == "color")
                                if (const Node* v = p->child("value"))
                                    tr.t.color = QColor(v->num("red"), v->num("green"), v->num("blue"));
                    if (lastEl && lastEl->name == "clipitem" && !trAfter.isEmpty() && !tr.fromBlack) trAfter.last() = int(trs.size());
                    trs << tr;
                    lastEl = &el;
                    continue;
                }
                if (el.name == "generatoritem") {
                    generators << el.at("name");
                    lastEl = &el;
                    continue;
                }
                if (el.name != "clipitem") continue;
                Item it;
                it.name = el.at("name");
                it.enabled = el.at("enabled").toUpper() != "FALSE";
                const int in = el.num("in"), out = el.num("out");
                it.in = in;
                it.length = std::max(0, out - in);
                const int start = el.num("start", -1), end = el.num("end", -1);
                trBefore << (lastEl && lastEl->name == "transitionitem" && !trs.last().toBlack ? int(trs.size()) - 1 : -1);
                trAfter << -1;
                it.start = start >= 0 ? start : end >= 0 ? end - it.length : trBefore.last() >= 0 ? trs[trBefore.last()].cut : 0;
                if (const Node* f = el.child("file")) {
                    const Node* def = files.value(f->attrs.value("id"), f);
                    const QString url = def->at("pathurl");
                    it.path = url.isEmpty() ? def->at("name") : pathFromUrl(url);
                    if (const int d = def->num("duration"); d > 0) it.mediaLength = d;
                }
                if (it.name.isEmpty()) it.name = QFileInfo(it.path).fileName();
                if (!video) it.audioStream = std::max(0, el.num("sourcetrack/trackindex", 1) - 1);
                for (const Node* filter : el.all("filter")) {
                    const Node* eff = filter->child("effect");
                    if (!eff) continue;
                    const QString id = eff->at("effectid").toLower();
                    const auto param = [eff](const QString& pid) -> const Node* {
                        for (const Node* p : eff->all("parameter"))
                            if (p->at("parameterid").toLower() == pid) return p;
                        return nullptr;
                    };
                    const auto keys = [](const Node* p, const std::function<double(double)>& convert) {
                        QVector<Key> out;
                        for (const Node* k : p->all("keyframe")) out << Key{k->num("when"), convert(k->at("value").toDouble())};
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
                    } else if (id != "basic" && id != "audiopan") {
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
                td.items << it;
                lastEl = &el;
            }
            // Transitions to items: a dissolve between the item before and after it, fades on one side
            for (int t = 0; t < trs.size(); ++t) {
                Transition tr = trs[t].t;
                tr.start = trs[t].start;
                tr.end = trs[t].end;
                tr.left = int(trAfter.indexOf(t));
                tr.right = int(trBefore.indexOf(t));
                if (trs[t].fromBlack) {
                    // the item after the fade in
                    for (int i = 0; i < td.items.size(); ++i)
                        if (td.items[i].start == tr.start) tr.right = i;
                    tr.left = -1;
                }
                if (trs[t].toBlack) {
                    for (int i = 0; i < td.items.size(); ++i)
                        if (td.items[i].start + td.items[i].length == tr.end) tr.left = i;
                    tr.right = -1;
                }
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
    if (otherTransitions) seq.warnings << T("Andere Übergänge als Blenden als Überblendung gelesen");
    return seq;
}

} // namespace Interchange
