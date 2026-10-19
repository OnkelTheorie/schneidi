// schneidi-cli: timelines to and from other editors (EDL, FCP7 XML, OpenTimelineIO), see core/Interchange.h
#include "cli/CommandsDetail.h"

#include "core/Interchange.h"
#include "core/TimelineOps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

namespace Cli::detail {

namespace {

Interchange::Format formatArg(const QJsonObject& a, const QString& file)
{
    const QString f = a["format"].toString().toLower();
    if (f == "edl") return Interchange::Format::Edl;
    if (f == "xml" || f == "fcp7" || f == "fcpxml7") return Interchange::Format::Fcp7;
    if (f == "otio") return Interchange::Format::Otio;
    if (!f.isEmpty()) fail("BAD_ARGUMENT", "format must be edl, xml (FCP7) or otio");
    if (const auto byName = Interchange::formatOf(file)) return *byName;
    fail("BAD_ARGUMENT", "cannot tell the format from the file name (.edl, .xml, .otio); give 'format'");
}

} // namespace

QJsonObject cmdExportTimeline(const QJsonObject& a, Context&)
{
    Session s(a["project"].toString());
    s.selectTimeline(a["timeline"]);
    QString out = absolute(a["out"].toString());
    const Interchange::Format f = formatArg(a, out);
    if (QFileInfo(out).suffix().isEmpty()) out += "." + Interchange::extension(f);
    if (QFileInfo::exists(out) && !a["overwrite"].toBool()) fail("EXISTS", "file exists (overwrite: true replaces it): " + out);
    Interchange::Sequence seq = Interchange::fromTimeline(s.project, s.project.currentSequence());
    const QByteArray data = Interchange::write(seq, f);
    QDir().mkpath(QFileInfo(out).absolutePath());
    QFile file(out);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) fail("WRITE_FAILED", "cannot write " + out);
    int clips = 0;
    for (const QVector<Interchange::TrackData>* tracks : {&seq.video, &seq.audio})
        for (const Interchange::TrackData& td : *tracks) clips += int(td.items.size());
    QJsonObject o{{"file", out}, {"format", Interchange::formatName(f)}, {"timeline", seq.name}, {"clips", clips}};
    if (!seq.warnings.isEmpty()) o["warnings"] = QJsonArray::fromStringList(seq.warnings);
    return o;
}

// import with timeline_file: a new timeline from another program's file, its media imported
QJsonObject importTimeline(Session& s, const QJsonObject& a)
{
    const QString path = absolute(a["timeline_file"].toString());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("NOT_FOUND", "cannot read " + path);
    const Interchange::Format f = formatArg(a, path);
    const FrameRate edlRate = a.contains("fps") ? nearestFrameRate(a["fps"].toDouble()) : s.format().rate;
    Interchange::Sequence seq;
    try {
        seq = Interchange::read(file.readAll(), f, edlRate);
    } catch (const Interchange::Error& e) {
        fail("BAD_FILE", Interchange::formatName(f) + ": " + e.message);
    }
    QStringList extra; // besides the warnings of the file
    // Format: an empty project takes the file's (like opening it in DaVinci), else the timeline is converted
    const ProjectFormat before = s.format();
    if (!s.project.frameRateLocked() && (seq.format.rate != before.rate || (seq.hasSize && seq.format.size() != before.size()))) {
        QJsonObject op{{"op", "format"}, {"fps", seq.format.rate.fps()}};
        if (seq.hasSize) op["width"] = seq.format.width, op["height"] = seq.format.height;
        applyOp(s, op);
    }
    // Media: as given, or found by name in the search folders (default: the folder of the file)
    QStringList folders{QFileInfo(path).absolutePath()};
    for (const QJsonValue& v : a["search"].toArray()) folders << absolute(v.toString());
    if (a["search"].isString()) folders << absolute(a["search"].toString());
    QHash<QString, QString> local;
    for (const QString& m : Interchange::mediaFiles(seq)) {
        const QString file = Interchange::findMedia(m, folders);
        if (file.isEmpty()) continue;
        try {
            s.media(file);
            local.insert(m, file);
        } catch (const Error&) {
            // not readable: counts as missing
        }
    }
    const Interchange::Imported imp = Interchange::addToProject(s.project, seq, local, a["name"].toString());
    s.selectTimeline(imp.sequenceId, true);
    QJsonObject o{{"timeline", QJsonObject{{"id", imp.sequenceId}, {"name", imp.name}, {"clips", imp.clips}}},
                  {"format", Interchange::formatName(f)}};
    QStringList warnings = imp.warnings + extra;
    if (!imp.missing.isEmpty()) {
        o["missing"] = QJsonArray::fromStringList(imp.missing);
        warnings << "give 'search' (folders where the missing files are) and import again";
    }
    if (!warnings.isEmpty()) o["warnings"] = QJsonArray::fromStringList(warnings);
    return o;
}

} // namespace Cli::detail
