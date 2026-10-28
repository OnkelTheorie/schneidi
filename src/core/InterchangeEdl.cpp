// SPDX-License-Identifier: AGPL-3.0-only
// EDL (CMX3600): one video track and audio channels, cuts and dissolves. Source timecodes are the file's own
// (Item::mediaStart, 00:00:00:00 without one), the timeline starts at 01:00:00:00 (like DaVinci). Comment lines carry the clip name and the file
// ("* FROM CLIP NAME:", "* SOURCE FILE:"), which Premiere and DaVinci read when conforming.
#include "core/Interchange.h"

#include "core/I18n.h"
#include "core/Timecode.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>

namespace Interchange {

namespace {

QString edlTc(int frames, int fps) { return Timecode::format(std::max(0, frames), fps); }

// Reel names have at most 8 characters; the file is in the comment, so "AX" (auxiliary) like DaVinci
constexpr const char* kReel = "AX";

struct Line {
    QString reel, track, kind; // kind: C or D
    int dur = 0;
    int srcIn = 0, srcOut = 0, recIn = 0, recOut = 0;
};

QString eventLine(int number, const Line& l, int fps)
{
    return QString("%1  %2 %3 %4 %5 %6 %7 %8 %9")
        .arg(number, 3, 10, QChar('0'))
        .arg(l.reel.leftJustified(8, ' ', true))
        .arg(l.track.leftJustified(5))
        .arg(l.kind.leftJustified(4))
        .arg(l.kind == "C" ? QString("   ") : QString("%1").arg(l.dur, 3, 10, QChar('0')))
        .arg(edlTc(l.srcIn, fps), edlTc(l.srcOut, fps), edlTc(l.recIn, fps), edlTc(l.recOut, fps));
}

} // namespace

QByteArray writeEdl(Sequence& seq)
{
    const int fps = seq.format.rate.timebase();
    const int offset = 3600 * fps; // record timecode of timeline frame 0
    QString out;
    QTextStream ts(&out);
    ts << "TITLE: " << seq.name << "\n";
    ts << "FCM: NON-DROP FRAME\n\n";
    int number = 0;
    bool dropped = false, reversed = false;
    // Track field per track: V; A, A2, A3, A4 (more audio tracks do not fit)
    QVector<QPair<const TrackData*, QString>> tracks;
    for (int i = 0; i < seq.video.size(); ++i)
        if (!seq.video[i].items.isEmpty()) {
            if (tracks.isEmpty()) tracks << qMakePair(&seq.video[i], QString("V"));
            else dropped = true;
        }
    for (int i = 0; i < seq.audio.size(); ++i) {
        if (seq.audio[i].items.isEmpty()) continue;
        if (i < 4) tracks << qMakePair(&seq.audio[i], i == 0 ? QString("A") : QString("A%1").arg(i + 1));
        else dropped = true;
    }
    for (const auto& [td, field] : tracks) {
        // Transitions by item: dissolve/fade in at the start of an item, fade out at its end
        QHash<int, const Transition*> inOf, outOf;
        for (const Transition& tr : td->transitions) {
            if (tr.right >= 0) inOf.insert(tr.right, &tr);
            else if (tr.left >= 0) outOf.insert(tr.left, &tr);
        }
        const auto src = [](const Item& it, int recFrame) { // source timecode at a timeline frame
            return it.mediaStart + int(std::lround((it.in + (recFrame - it.start)) * it.speed));
        };
        for (int i = 0; i < td->items.size(); ++i) {
            const Item& it = td->items[i];
            reversed = reversed || it.reverse;
            // The item plays from `from` (a dissolve into it starts there) to `to` (a dissolve out of it ends earlier,
            // the next event takes over)
            const Transition* in = inOf.value(i);
            int from = in ? in->start : it.start;
            int to = it.start + it.length;
            if (i + 1 < td->items.size())
                if (const Transition* next = inOf.value(i + 1); next && next->left == i) to = std::min(to, next->start);
            const Transition* fadeOut = outOf.value(i);
            if (fadeOut) to = std::min(to, fadeOut->start);
            Line l{kReel, field, "C", 0, src(it, from), src(it, to), offset + from, offset + to};
            ++number;
            if (in) {
                // Two lines: what plays before (the outgoing clip or black) for 0 frames, then the dissolve
                Line prev{"BL", field, "C", 0, 0, 0, offset + in->start, offset + in->start};
                if (in->left >= 0) {
                    const Item& left = td->items[in->left];
                    prev.reel = kReel;
                    prev.srcIn = prev.srcOut = src(left, in->start);
                }
                ts << eventLine(number, prev, fps) << "\n";
                l.kind = "D";
                l.dur = in->end - in->start;
            }
            ts << eventLine(number, l, fps) << "\n";
            if (it.speed != 1.0)
                ts << QString("M2   %1 %2                %3\n")
                          .arg(QString(kReel).leftJustified(8))
                          .arg(QString::number(fps * it.speed, 'f', 1).rightJustified(5, '0'))
                          .arg(edlTc(l.srcIn, fps));
            if (in && in->left >= 0) ts << "* FROM CLIP NAME: " << td->items[in->left].name << "\n* TO CLIP NAME: " << it.name << "\n";
            else ts << "* FROM CLIP NAME: " << it.name << "\n";
            ts << "* SOURCE FILE: " << QFileInfo(it.path).absoluteFilePath() << "\n";
            if (fadeOut) {
                // Fade to black: the clip for 0 frames, then black dissolving in
                ts << "\n";
                ++number;
                const int s = src(it, fadeOut->start);
                ts << eventLine(number, Line{kReel, field, "C", 0, s, s, offset + fadeOut->start, offset + fadeOut->start}, fps) << "\n";
                ts << eventLine(number, Line{"BL", field, "D", fadeOut->end - fadeOut->start, 0, fadeOut->end - fadeOut->start,
                                             offset + fadeOut->start, offset + fadeOut->end}, fps)
                   << "\n";
            }
            ts << "\n";
        }
    }
    if (dropped) seq.warnings << T("EDL: nur die erste Videospur und die Audiospuren A1–A4 geschrieben");
    if (reversed) seq.warnings << T("EDL: rückwärts laufende Clips vorwärts geschrieben");
    if (!seq.markers.isEmpty()) seq.warnings << T("EDL: Marker nicht geschrieben");
    bool keys = false;
    for (const QVector<TrackData>* list : {&seq.video, &seq.audio})
        for (const TrackData& td : *list)
            for (const Item& it : td.items)
                keys = keys || it.volumeDb != 0 || it.opacity != 100 || !it.volumeKeys.isEmpty() || !it.opacityKeys.isEmpty();
    if (keys) seq.warnings << T("EDL: Lautstärke und Deckkraft nicht geschrieben");
    return out.toUtf8();
}

Sequence readEdl(const QByteArray& data, FrameRate rate)
{
    Sequence seq;
    seq.format.rate = rate;
    const int fps = rate.timebase();
    static const QRegularExpression eventRe(
        R"(^\s*(\d+)\s+(\S+)\s+(\S+)\s+(C|D|W\d*|K\s*[BO]?)\s+(\d+\s+)?(\d\d[:;.]\d\d[:;.]\d\d[:;.]\d\d)\s+(\d\d[:;.]\d\d[:;.]\d\d[:;.]\d\d)\s+(\d\d[:;.]\d\d[:;.]\d\d[:;.]\d\d)\s+(\d\d[:;.]\d\d[:;.]\d\d[:;.]\d\d))");
    static const QRegularExpression m2Re(R"(^\s*M2\s+(\S+)\s+(-?[\d.]+)\s+(\d\d[:;.]\d\d[:;.]\d\d[:;.]\d\d))");
    // One parsed event (a cut, or the second line of a dissolve)
    struct Event {
        QString reel, field;
        bool dissolve = false;
        int dur = 0, srcIn = 0, srcOut = 0, recIn = 0, recOut = 0;
        double speed = 1;
        bool reverse = false;
        QString clipName, file;
        bool afterBlack = false; // dissolve from black (fade in)
    };
    QVector<Event> events;
    Event* last = nullptr;
    bool wipes = false;
    const QStringList lines = QString::fromUtf8(data).split(QRegularExpression("\r?\n|\r"));
    for (const QString& line : lines) {
        if (line.startsWith("TITLE:")) {
            seq.name = line.mid(6).trimmed();
            continue;
        }
        if (const auto m = eventRe.match(line); m.hasMatch()) {
            Event e;
            e.reel = m.captured(2);
            e.field = m.captured(3).toUpper();
            const QString kind = m.captured(4).toUpper();
            e.dissolve = kind != "C";
            wipes = wipes || kind.startsWith('W') || kind.startsWith('K');
            e.dur = m.captured(5).trimmed().toInt();
            e.srcIn = Timecode::parse(m.captured(6), fps);
            e.srcOut = Timecode::parse(m.captured(7), fps);
            e.recIn = Timecode::parse(m.captured(8), fps);
            e.recOut = Timecode::parse(m.captured(9), fps);
            // Second line of a dissolve: the line before (same event, 0 frames) is the outgoing clip or black
            if (e.dissolve && last && last->recIn == last->recOut && m.captured(1).toInt() > 0) {
                e.afterBlack = last->reel.toUpper() == "BL" || last->reel.toUpper() == "BLK";
                events.removeLast();
            }
            events << e;
            last = &events.last();
            continue;
        }
        if (const auto m = m2Re.match(line); m.hasMatch() && last) {
            const double v = m.captured(2).toDouble() / fps;
            if (v != 0) {
                last->speed = std::abs(v);
                last->reverse = v < 0;
            }
            continue;
        }
        const QString t = line.trimmed();
        if (!last || !t.startsWith('*')) continue;
        const QString body = t.mid(1).trimmed();
        const auto value = [&body](const char* key) {
            return body.startsWith(key, Qt::CaseInsensitive) ? body.mid(int(qstrlen(key))).trimmed() : QString();
        };
        if (QString v = value("FROM CLIP NAME:"); !v.isEmpty() && last->clipName.isEmpty()) last->clipName = v;
        if (QString v = value("TO CLIP NAME:"); !v.isEmpty()) last->clipName = v; // the incoming clip of a dissolve
        if (QString v = value("SOURCE FILE:"); !v.isEmpty()) last->file = pathFromUrl(v);
    }
    if (events.isEmpty()) throw Error{T("Keine EDL-Ereignisse gefunden")};
    // Timeline start: the hour of the first event (01:00:00:00 in most programs), or the first event when that is far
    // from a full hour (Premiere: 00:59:53:11)
    int first = INT_MAX;
    for (const Event& e : events) first = std::min(first, e.recIn);
    int offset = first / (3600 * fps) * (3600 * fps);
    if (first - offset > 600 * fps) offset = first;
    seq.inIsTimecode = true;
    int link = 0;
    for (const Event& e : events) {
        const bool black = e.reel.toUpper() == "BL" || e.reel.toUpper() == "BLK";
        if (black && !e.dissolve) continue; // black filler
        QVector<QPair<TrackKind, int>> targets;
        const QString f = e.field;
        if (f.contains('V') || f == "B") targets << qMakePair(TrackKind::Video, 0);
        if (f == "A" || f == "B" || f == "AA" || f.startsWith("AA/") || f.startsWith("A/")) targets << qMakePair(TrackKind::Audio, 0);
        if (f == "AA" || f.startsWith("AA/")) targets << qMakePair(TrackKind::Audio, 1);
        static const QRegularExpression an("^A(\\d+)$");
        if (const auto m = an.match(f); m.hasMatch() && m.captured(1).toInt() >= 2) targets << qMakePair(TrackKind::Audio, m.captured(1).toInt() - 1);
        if (targets.isEmpty()) continue;
        ++link;
        for (const auto& [kind, index] : targets) {
            QVector<TrackData>& tracks = kind == TrackKind::Video ? seq.video : seq.audio;
            while (tracks.size() <= index)
                tracks << TrackData{};
            TrackData& td = tracks[index];
            const int recIn = e.recIn - offset, recOut = e.recOut - offset;
            if (black) {
                // Black dissolving in = fade out of the item that ends where the black starts
                for (int i = int(td.items.size()) - 1; i >= 0; --i)
                    if (td.items[i].start + td.items[i].length == recIn) {
                        td.transitions << Transition{i, -1, recIn, recIn + e.dur};
                        // the clip plays on under the fade (it was cut at the fade start in the EDL)
                        td.items[i].length += e.dur;
                        break;
                    }
                continue;
            }
            Item it;
            it.path = !e.file.isEmpty() ? e.file : !e.clipName.isEmpty() ? e.clipName : e.reel;
            it.name = !e.clipName.isEmpty() ? e.clipName : QFileInfo(it.path).fileName();
            it.start = recIn;
            it.length = recOut - recIn;
            it.speed = e.speed;
            it.reverse = e.reverse;
            it.in = int(std::lround(e.srcIn / e.speed));
            it.link = targets.size() > 1 ? link : 0;
            const int added = int(td.items.size());
            td.items << it;
            if (e.dissolve) {
                int left = -1;
                if (!e.afterBlack)
                    for (int i = added - 1; i >= 0; --i)
                        if (td.items[i].start + td.items[i].length == recIn) {
                            left = i;
                            break;
                        }
                td.transitions << Transition{left, added, recIn, recIn + e.dur};
            }
        }
    }
    for (QVector<TrackData>* tracks : {&seq.video, &seq.audio})
        for (TrackData& td : *tracks) {
            // Items sorted by start (EDL events are in record order per track anyway)
            bool sorted = std::is_sorted(td.items.begin(), td.items.end(), [](const Item& a, const Item& b) { return a.start < b.start; });
            if (!sorted) seq.warnings << T("EDL: Ereignisse auf %1 nicht in zeitlicher Reihenfolge").arg(td.name);
        }
    if (wipes) seq.warnings << T("EDL: Wischblenden und Keys als Überblendung gelesen");
    seq.warnings << T("EDL: keine Framerate in der Datei, mit %1 fps gelesen").arg(rate.label());
    return seq;
}

} // namespace Interchange
