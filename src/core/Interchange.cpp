#include "core/Interchange.h"

#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <climits>
#include <cmath>

namespace Interchange {

std::optional<Format> formatOf(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == "edl") return Format::Edl;
    if (ext == "xml" || ext == "fcpxml7") return Format::Fcp7;
    if (ext == "otio") return Format::Otio;
    return std::nullopt;
}

QString extension(Format f)
{
    switch (f) {
    case Format::Edl: return "edl";
    case Format::Fcp7: return "xml";
    case Format::Otio: return "otio";
    }
    return {};
}

QString formatName(Format f)
{
    switch (f) {
    case Format::Edl: return "EDL (CMX3600)";
    case Format::Fcp7: return "Final Cut Pro 7 XML";
    case Format::Otio: return "OpenTimelineIO";
    }
    return {};
}

namespace {

QVector<Key> keysOf(const Clip& c, AnimParam p)
{
    QVector<Key> out;
    for (const Keyframe& k : c.keys.value(p)) out << Key{k.frame, k.value};
    return out;
}

// "3 clips (a.mp4, b.mp4, …)"
QString clipList(const QStringList& names)
{
    QStringList unique = names;
    unique.removeDuplicates();
    QStringList shown = unique.mid(0, 3);
    if (unique.size() > 3) shown << "…";
    if (names.size() == 1) return T("1 Clip (%1)").arg(shown.join(", "));
    return T("%1 Clips (%2)").arg(names.size()).arg(shown.join(", "));
}

} // namespace

Sequence fromTimeline(const Project& project, int sequenceId)
{
    const int id = sequenceId > 0 ? sequenceId : project.currentSequence();
    const ::Sequence* s = project.sequence(id);
    Sequence out;
    out.name = project.sequenceName(id);
    out.format = project.format();
    out.hasSize = true;
    if (!s) return out;
    const Timeline& tl = s->timeline;
    out.markers = tl.markers;
    QStringList skipped, ramps, freezes, effects, transforms, pans, wipes;
    const TimelineOps::SourceLength sourceLength = [&project](const Clip& c) {
        const MediaInfo* m = c.isNested() ? nullptr : project.mediaInfo(c.mediaPath);
        return m && !m->isImage ? c.retimedLength(m->length) : 0;
    };
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<Track>& tracks = tl.tracks(kind);
        for (int t = 0; t < tracks.size(); ++t) {
            const Track& track = tracks[t];
            TrackData td;
            td.name = track.name; // empty = the default name
            td.enabled = kind == TrackKind::Video ? !track.hidden : !track.muted;
            QHash<int, int> indexOf; // clip id -> item index
            QVector<QPair<int, int>> fades; // item index -> fade handle in/out (frames)
            for (const Clip& c : track.clips) {
                const QString name = project.clipName(c);
                if (c.isTitle() || c.isNested() || c.mediaPath.isEmpty()) {
                    skipped << name;
                    continue;
                }
                Item it;
                it.path = c.mediaPath;
                it.name = QFileInfo(c.mediaPath).fileName();
                it.start = c.start;
                it.length = c.length();
                it.in = c.in;
                it.speed = c.speed;
                it.reverse = c.reverse;
                it.enabled = c.enabled;
                it.link = c.linkId;
                if (const MediaInfo* m = project.mediaInfo(c.mediaPath)) it.mediaLength = m->isImage ? 0 : m->length;
                if (c.hasRamp()) ramps << name;
                if (c.freeze) {
                    freezes << name;
                    it.in = c.stillFrame();
                }
                if (kind == TrackKind::Audio) {
                    it.audioStream = c.audioStream;
                    it.volumeDb = c.volumeDb;
                    it.volumeKeys = keysOf(c, AnimParam::Volume);
                    if (c.pan != 0 || c.keys.contains(AnimParam::Pan)) pans << name;
                } else {
                    it.opacity = c.transform.opacity;
                    it.opacityKeys = keysOf(c, AnimParam::Opacity);
                    if (Keys::hasTransform(c)) transforms << name;
                }
                if (!c.effects.isEmpty()) effects << name;
                indexOf.insert(c.id, int(td.items.size()));
                fades << qMakePair(std::min(c.fadeIn, c.length()), std::min(c.fadeOut, c.length()));
                td.items << it;
            }
            for (const TimelineOps::TransitionSpan& sp : TimelineOps::transitions(track, sourceLength)) {
                const int l = sp.leftId ? indexOf.value(sp.leftId, -2) : -1;
                const int r = sp.rightId ? indexOf.value(sp.rightId, -2) : -1;
                if (l == -2 || r == -2) continue; // a clip that was left out
                Transition tr{l, r, sp.start, sp.end, sp.style.type, sp.style.color};
                if (tr.type != TransitionType::CrossDissolve && tr.type != TransitionType::DipToColor) {
                    wipes << (l >= 0 ? td.items[l].name : td.items[r].name);
                    tr.type = TransitionType::CrossDissolve;
                }
                td.transitions << tr;
            }
            // Fade handles (opacity/volume at the clip edges) as a fade where the edge has no transition: in other
            // programs a transition at one edge fades the clip in or out
            for (int i = 0; i < td.items.size(); ++i) {
                const Item& it = td.items[i];
                bool atIn = false, atOut = false;
                for (const Transition& tr : td.transitions) {
                    atIn = atIn || tr.right == i;
                    atOut = atOut || tr.left == i;
                }
                if (fades[i].first > 0 && !atIn) td.transitions << Transition{-1, i, it.start, it.start + fades[i].first};
                if (fades[i].second > 0 && !atOut)
                    td.transitions << Transition{i, -1, it.start + it.length - fades[i].second, it.start + it.length};
            }
            (kind == TrackKind::Video ? out.video : out.audio) << td;
        }
    }
    const auto warn = [&out](const QStringList& names, const QString& what) {
        if (!names.isEmpty()) out.warnings << clipList(names) + ": " + what;
    };
    warn(skipped, T("Titel, Compound- und Multicam-Clips weggelassen"));
    warn(ramps, T("Geschwindigkeitsrampe als erste Geschwindigkeit des Clips geschrieben"));
    warn(freezes, T("Standbild als normaler Clip geschrieben"));
    warn(effects, T("Effekte und Farbkorrektur nicht übernommen"));
    warn(transforms, T("Position, Zoom, Drehung und Beschnitt nicht übernommen"));
    warn(pans, T("Panorama nicht übernommen"));
    warn(wipes, T("Wischblenden als Überblendung geschrieben"));
    if (!tl.subtitles.isEmpty()) out.warnings << T("Untertitelspuren nicht übernommen (als SRT exportieren)");
    return out;
}

Timeline toTimeline(const Sequence& seq, const std::function<int()>& newId, const std::function<int()>& newLinkId)
{
    Timeline tl;
    tl.markers = seq.markers;
    std::sort(tl.markers.begin(), tl.markers.end());
    tl.markers.erase(std::unique(tl.markers.begin(), tl.markers.end()), tl.markers.end());
    QHash<int, int> links; // link of the file -> link id
    for (const TrackKind kind : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<TrackData>& tracks = kind == TrackKind::Video ? seq.video : seq.audio;
        QVector<Track>& out = tl.tracks(kind);
        for (const TrackData& td : tracks) {
            Track track;
            track.kind = kind;
            track.name = td.name;
            if (kind == TrackKind::Video) track.hidden = !td.enabled;
            else track.muted = !td.enabled;
            QVector<int> clipOfItem(td.items.size(), -1); // index in track.clips
            for (int i = 0; i < td.items.size(); ++i) {
                const Item& it = td.items[i];
                if (it.length <= 0) continue;
                Clip c;
                c.id = newId();
                c.mediaPath = it.path;
                c.start = it.start;
                c.in = it.in;
                c.out = it.in + it.length - 1;
                c.speed = it.speed > 0 ? it.speed : 1.0;
                c.reverse = it.reverse;
                c.enabled = it.enabled;
                if (it.link > 0) {
                    if (!links.contains(it.link)) links.insert(it.link, newLinkId());
                    c.linkId = links.value(it.link);
                }
                const auto keys = [&c](AnimParam p, const QVector<Key>& list) {
                    for (const Key& k : list) c.keys[p] << Keyframe{k.frame, k.value};
                };
                if (kind == TrackKind::Audio) {
                    c.audioStream = it.audioStream;
                    c.volumeDb = it.volumeDb;
                    keys(AnimParam::Volume, it.volumeKeys);
                } else {
                    c.transform.opacity = it.opacity;
                    keys(AnimParam::Opacity, it.opacityKeys);
                }
                // Overlapping items (not allowed on a schneidi track): the later one starts where the earlier ends
                if (!track.clips.isEmpty() && c.start < track.clips.last().end()) {
                    const int cut = track.clips.last().end() - c.start;
                    if (cut >= c.length()) continue;
                    c.start += cut;
                    c.in += cut;
                }
                clipOfItem[i] = int(track.clips.size());
                track.clips << c;
            }
            // Transitions: lengths at the clip edges, alignment from where the cut is
            const auto clipAt = [&](int item) -> Clip* {
                const int i = item >= 0 && item < clipOfItem.size() ? clipOfItem[item] : -1;
                return i >= 0 ? &track.clips[i] : nullptr;
            };
            for (const Transition& tr : td.transitions) {
                Clip* l = clipAt(tr.left);
                Clip* r = clipAt(tr.right);
                const int len = tr.end - tr.start;
                if (len <= 0 || (!l && !r)) continue;
                TransitionStyle style;
                style.type = tr.type;
                style.color = tr.color;
                if (l && r) {
                    if (l->end() != r->start) continue; // a dissolve needs the clips side by side
                    const int before = l->end() - tr.start;
                    style.align = before <= 0 ? TransitionAlign::Start : before >= len ? TransitionAlign::End : TransitionAlign::Center;
                    l->transOut = r->transIn = len;
                    l->transOutStyle = r->transInStyle = style;
                } else if (r) {
                    r->transIn = len;
                    r->transInStyle = style;
                } else {
                    l->transOut = len;
                    l->transOutStyle = style;
                }
            }
            out << track;
        }
    }
    // schneidi always has at least one video and one audio track
    if (tl.video.isEmpty()) tl.video << Track{TrackKind::Video};
    if (tl.audio.isEmpty()) tl.audio << Track{TrackKind::Audio};
    return tl;
}

void convertRate(Sequence& seq, FrameRate rate)
{
    const double k = rate.fps() / seq.format.rate.fps();
    seq.format.rate = rate;
    if (std::abs(k - 1.0) < 1e-9) return;
    const auto f = [k](int v) { return int(std::lround(v * k)); };
    for (QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (TrackData& td : *tracks) {
            for (Item& it : td.items) {
                const int end = f(it.start + it.length);
                it.start = f(it.start);
                it.length = it.length > 0 ? std::max(1, end - it.start) : 0; // 0 = left out (fitAudioStreams)
                it.in = f(it.in);
                it.mediaStart = f(it.mediaStart);
                for (Key& key : it.volumeKeys) key.frame = f(key.frame);
                for (Key& key : it.opacityKeys) key.frame = f(key.frame);
            }
            for (Transition& tr : td.transitions) {
                tr.start = f(tr.start);
                tr.end = f(tr.end);
            }
        }
    for (int& m : seq.markers) m = f(m);
}

void fitAudioStreams(Sequence& seq, const std::function<int(const QString&)>& streams)
{
    int dropped = 0;
    for (TrackData& td : seq.audio)
        for (Item& it : td.items) {
            const int n = streams(it.path);
            if (n <= 0 || it.audioStream < n) continue;
            // Its twin: the item of a stream the file has, at the same place or of the same link (Premiere trims
            // the channels of a clip one by one at audio transitions)
            bool twin = false;
            for (const TrackData& other : seq.audio)
                for (const Item& o : other.items)
                    twin = twin || (&o != &it && o.path == it.path && o.audioStream < n && o.length > 0
                                    && ((o.start == it.start && o.length == it.length)
                                        || (it.link > 0 && o.link == it.link && o.start < it.start + it.length && it.start < o.start + o.length)));
            if (twin) {
                it.length = 0;
                ++dropped;
            } else {
                it.audioStream = 0;
            }
        }
    if (dropped)
        seq.warnings << T("%1 Audioclips einzelner Kanäle weggelassen (der Stream der Datei spielt alle Kanäle)").arg(dropped);
}

void applyStartTimecodes(Sequence& seq, const std::function<QString(const QString&)>& timecode,
                         const std::function<int(const QString&)>& length)
{
    const int fps = seq.format.rate.timebase();
    const int hour = 3600 * fps;
    QHash<QString, int> startOf; // path -> start frame, -1 = no timecode
    for (const QString& path : mediaFiles(seq)) {
        const QString tc = timecode ? timecode(path) : QString();
        startOf.insert(path, tc.isEmpty() ? -1 : std::max(-1, Timecode::parse(tc, fps)));
    }
    const auto fileFrame = [](const Item& it) { return int(std::lround(it.in * it.speed)); };
    if (seq.inIsTimecode && length) {
        // No timecode, every clip beyond the end of the file: the full hours of the source timecodes are taken off
        QHash<QString, int> first;
        QSet<QString> inside;
        for (const QVector<TrackData>* tracks : {&seq.video, &seq.audio})
            for (const TrackData& td : *tracks)
                for (const Item& it : td.items) {
                    if (startOf.value(it.path) >= 0) continue;
                    const int n = length(it.path);
                    if (n <= 0 || fileFrame(it) < n) inside << it.path;
                    first[it.path] = std::min(first.value(it.path, INT_MAX), fileFrame(it));
                }
        for (auto i = first.cbegin(); i != first.cend(); ++i)
            if (!inside.contains(i.key()) && i.value() >= hour) startOf[i.key()] = i.value() / hour * hour;
    }
    for (QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (TrackData& td : *tracks)
            for (Item& it : td.items) {
                const int start = std::max(0, startOf.value(it.path, -1));
                if (seq.inIsTimecode) it.in = int(std::lround(std::max(0, fileFrame(it) - start) / it.speed));
                it.mediaStart = start;
            }
    seq.inIsTimecode = false;
}

QStringList mediaFiles(const Sequence& seq)
{
    QStringList out;
    for (const QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (const TrackData& td : *tracks)
            for (const Item& it : td.items)
                if (!it.path.isEmpty() && it.length > 0 && !out.contains(it.path)) out << it.path;
    return out;
}

QString findMedia(const QString& path, const QStringList& folders)
{
    if (QFileInfo::exists(path)) return QFileInfo(path).absoluteFilePath();
    const QString name = QFileInfo(QString(path).replace('\\', '/')).fileName();
    if (name.isEmpty()) return {};
    for (const QString& folder : folders) {
        const QString direct = QDir(folder).filePath(name);
        if (QFileInfo::exists(direct)) return QFileInfo(direct).absoluteFilePath();
        QDirIterator it(folder, {name}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString found = it.next();
            if (QDir(folder).relativeFilePath(found).count('/') <= 3) return found;
        }
    }
    return {};
}

Imported addToProject(Project& p, Sequence seq, const QHash<QString, QString>& local, const QString& name,
                      const std::function<QString(const QString&)>& timecode)
{
    Imported out;
    int beyond = 0; // clips that lie behind the end of their file
    if (seq.inIsTimecode) {
        const double k = seq.format.rate.fps() / p.format().rate.fps();
        applyStartTimecodes(
            seq,
            [&](const QString& path) {
                const QString file = local.value(path, path);
                return timecode && p.mediaInfo(file) ? timecode(file) : QString();
            },
            [&](const QString& path) {
                const MediaInfo* m = p.mediaInfo(local.value(path, path));
                return m && !m->isImage ? int(m->length * k) : 0;
            });
    }
    for (QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (TrackData& td : *tracks)
            for (Item& it : td.items) {
                const QString file = local.value(it.path, it.path);
                const MediaInfo* m = p.mediaInfo(file);
                if (!m) {
                    if (it.length > 0 && !out.missing.contains(it.path)) out.missing << it.path;
                    it.length = 0; // left out
                    continue;
                }
                it.path = file;
                // Beyond the end of the file (another version of it, another frame rate): shortened
                if (!m->isImage) {
                    const double fileLength = m->length * seq.format.rate.fps() / p.format().rate.fps() / std::max(0.01, it.speed);
                    if (it.in + it.length > fileLength) {
                        it.length = std::max(0, int(fileLength) - it.in);
                        if (it.length == 0) ++beyond;
                    }
                }
            }
    fitAudioStreams(seq, [&p](const QString& path) {
        const MediaInfo* m = p.mediaInfo(path);
        return m ? m->audioStreamCount() : 0;
    });
    if (seq.format.rate != p.format().rate)
        seq.warnings << T("Von %1 auf die %2 fps des Projekts umgerechnet").arg(seq.format.rate.label(), p.format().rate.label());
    convertRate(seq, p.format().rate);
    if (!out.missing.isEmpty())
        seq.warnings << T("%1 Mediendateien nicht gefunden, ihre Clips fehlen").arg(out.missing.size());
    if (beyond > 0)
        seq.warnings << T("%1 Clips liegen hinter dem Ende ihrer Datei und fehlen (andere Fassung der Datei oder anderer Timecode?)").arg(beyond);
    // Name unique like the app's timelines
    const QString base = !name.trimmed().isEmpty() ? name.trimmed() : !seq.name.isEmpty() ? seq.name : QString("Timeline");
    QStringList taken;
    for (const ::Sequence& q : p.sequences()) taken << q.name;
    out.name = base;
    for (int n = 2; taken.contains(out.name, Qt::CaseInsensitive); ++n) out.name = QString("%1 (%2)").arg(base).arg(n);
    const Timeline tl = toTimeline(seq, [&p] { return p.newClipId(); }, [&p] { return p.newLinkId(); });
    for (const TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k)) out.clips += int(t.clips.size());
    out.sequenceId = p.newSequenceId();
    p.editSequences(T("Timeline importieren"), [&](QVector<::Sequence>& list, int& current) {
        list << ::Sequence{out.sequenceId, out.name, false, 0, tl};
        current = out.sequenceId;
    });
    out.warnings = seq.warnings;
    return out;
}

QByteArray write(Sequence& seq, Format f)
{
    switch (f) {
    case Format::Edl: return writeEdl(seq);
    case Format::Fcp7: return writeFcp7(seq);
    case Format::Otio: return writeOtio(seq);
    }
    return {};
}

Sequence read(const QByteArray& data, Format f, FrameRate edlRate)
{
    switch (f) {
    case Format::Edl: return readEdl(data, edlRate);
    case Format::Fcp7: return readFcp7(data);
    case Format::Otio: return readOtio(data);
    }
    return {};
}

QString pathFromUrl(const QString& url)
{
    QString s = url.trimmed();
    if (s.startsWith("file:", Qt::CaseInsensitive)) {
        // file://localhost/C%3a/Users/… (Premiere), file:///home/… ; QUrl leaves "/C:/…" for Windows drives
        s.replace(QRegularExpression("^file://localhost/", QRegularExpression::CaseInsensitiveOption), "file:///");
        s = QUrl(s).toLocalFile();
        if (s.size() > 2 && s[0] == '/' && s[2] == ':' && s[1].isLetter()) s.remove(0, 1);
    }
    // A Windows path from another computer (C:\… or \\server\…): slashes, so the file name can be found
    if ((s.size() > 2 && s[1] == ':' && s[0].isLetter()) || s.startsWith("\\\\")) s.replace('\\', '/');
    return s;
}

QString urlFromPath(const QString& path)
{
    return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString(QUrl::FullyEncoded);
}

} // namespace Interchange
