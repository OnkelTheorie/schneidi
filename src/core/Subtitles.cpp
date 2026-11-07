// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Subtitles.h"

#include "core/Captions.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>

namespace Subtitles {

namespace {

int msToFrame(qint64 ms, double fps)
{
    return int(std::llround(ms * fps / 1000.0));
}

qint64 frameToMs(int frame, double fps)
{
    return std::llround(frame * 1000.0 / std::max(1e-6, fps));
}

QString decode(const QByteArray& data)
{
    QByteArray bytes = data;
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8(bytes);
    if (!utf8.hasError()) return text;
    // Older subtitles (Windows): 1252 = Latin-1 plus typographic characters in 0x80-0x9F
    static const char16_t cp1252[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
        0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    QString out;
    out.reserve(bytes.size());
    for (char ch : bytes) {
        const uchar u = uchar(ch);
        out += (u >= 0x80 && u < 0xA0) ? QChar(cp1252[u - 0x80]) : QChar(u);
    }
    return out;
}

// "01:02:03,456" (also "." and hours without leading zero) -> ms, -1 = invalid (also beyond 100000 hours: in ms
// they would not even fit into 64 bits)
qint64 parseTime(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d+):(\\d{1,2}):(\\d{1,2})(?:[,.](\\d{1,3}))?\\s*$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return -1;
    bool ok = false;
    const qint64 hours = m.captured(1).toLongLong(&ok);
    if (!ok || hours > 100000) return -1;
    QString frac = m.captured(4);
    while (frac.size() < 3) frac += '0'; // ",5" = 500 ms
    return ((hours * 60 + m.captured(2).toInt()) * 60 + m.captured(3).toInt()) * 1000 + frac.toInt();
}

QString stripFormatting(QString text)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    static const QRegularExpression ass(QStringLiteral("\\{\\\\[^}]*\\}"));
    text.remove(tags);
    text.remove(ass);
    return text.trimmed();
}

} // namespace

QVector<SrtFile> srtFiles(const Timeline& tl, const QString& path)
{
    int main = -1;
    for (int i = 0; i < tl.subtitles.size(); ++i)
        if (tl.subtitles[i].enabled && !tl.subtitles[i].cues.isEmpty()) main = i;
    for (int i = 0; i < tl.subtitles.size() && main < 0; ++i)
        if (!tl.subtitles[i].cues.isEmpty()) main = i;
    if (main < 0) return {};
    QVector<SrtFile> out{{path, main}};
    const QFileInfo fi(path);
    const QString suffix = fi.suffix().isEmpty() ? QString("srt") : fi.suffix();
    QSet<QString> taken; // lower case: Windows and macOS do not tell "En" from "en"
    static const QRegularExpression bad(R"([\\/:*?"<>|\x00-\x1f])");
    for (int i = 0; i < tl.subtitles.size(); ++i) {
        if (i == main || tl.subtitles[i].cues.isEmpty()) continue;
        QString name = QString(tl.subtitles[i].name).replace(bad, "_").trimmed();
        while (name.endsWith('.')) name.chop(1); // Windows drops trailing dots
        // a clash with another name (also a track named like a number): the track number, then counted up
        for (int n = i + 1; name.isEmpty() || taken.contains(name.toLower()); ++n) name = QString::number(n);
        taken.insert(name.toLower());
        out << SrtFile{fi.dir().filePath(QString("%1.%2.%3").arg(fi.completeBaseName(), name, suffix)), i};
    }
    return out;
}

QString srtTime(qint64 ms)
{
    ms = std::max<qint64>(0, ms);
    return QString("%1:%2:%3,%4")
        .arg(ms / 3600000, 2, 10, QChar('0'))
        .arg((ms / 60000) % 60, 2, 10, QChar('0'))
        .arg((ms / 1000) % 60, 2, 10, QChar('0'))
        .arg(ms % 1000, 3, 10, QChar('0'));
}

QVector<SubtitleCue> parseSrt(const QByteArray& data, double fps, int* skipped)
{
    QString text = decode(data);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n")).replace('\r', '\n');
    const QStringList lines = text.split('\n');
    QVector<SubtitleCue> cues;
    int bad = 0;
    for (int i = 0; i < lines.size();) {
        // Block: [number] / time line / text up to blank line
        while (i < lines.size() && lines[i].trimmed().isEmpty()) ++i;
        if (i >= lines.size()) break;
        if (!lines[i].contains(QLatin1String("-->")) && i + 1 < lines.size()) ++i; // number (also faulty ones)
        const QString timing = lines[i++];
        const int arrow = timing.indexOf(QLatin1String("-->"));
        QStringList body;
        while (i < lines.size() && !lines[i].trimmed().isEmpty()) body << lines[i++];
        if (arrow < 0) {
            ++bad;
            continue;
        }
        // Position info can follow the end time ("X1:... Y1:...")
        const qint64 from = parseTime(timing.left(arrow));
        const qint64 to = parseTime(timing.mid(arrow + 3).trimmed().section(' ', 0, 0));
        const QString t = stripFormatting(body.join('\n'));
        // Beyond 2^30 frames (years) the frame numbers would overflow: broken as well
        if (from < 0 || to < 0 || t.isEmpty() || std::max(from, to) * fps / 1000.0 > (1 << 30)) {
            ++bad;
            continue;
        }
        SubtitleCue c;
        c.start = msToFrame(from, fps);
        c.end = std::max(c.start + 1, msToFrame(to, fps));
        c.text = t;
        cues << c;
    }
    std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    // Overlaps (occur in foreign files): shorten the previous entry, merge duplicate start time
    QVector<SubtitleCue> out;
    for (const SubtitleCue& c : cues) {
        if (!out.isEmpty() && out.last().start == c.start) {
            out.last().text += '\n' + c.text;
            out.last().end = std::max(out.last().end, c.end);
            continue;
        }
        if (!out.isEmpty() && out.last().end > c.start) out.last().end = c.start;
        out << c;
    }
    if (skipped) *skipped = bad;
    return out;
}

QByteArray toSrt(const QVector<SubtitleCue>& cues, double fps, int offset, int until)
{
    QString out;
    int n = 0;
    for (const SubtitleCue& c : cues) {
        int start = c.start, end = c.end;
        if (until >= 0) end = std::min(end, until);
        start = std::max(start, offset);
        if (end <= start) continue;
        out += QString::number(++n) + "\r\n";
        out += srtTime(frameToMs(start - offset, fps)) + " --> " + srtTime(frameToMs(end - offset, fps)) + "\r\n";
        // A blank line ends an entry: empty lines inside the text (typed, or left by removed formatting) are left
        // out, else the rest of the text would become a broken entry of its own
        QStringList lines = c.text.trimmed().split('\n');
        lines.removeIf([](const QString& line) { return line.trimmed().isEmpty(); });
        out += lines.join(QLatin1String("\r\n")) + "\r\n\r\n";
    }
    return out.toUtf8();
}

namespace {
// The cues of [offset, until) with their lines (blank lines left out: a blank line ends an entry in SRT and VTT)
template<typename Fn>
void forCues(const QVector<SubtitleCue>& cues, int offset, int until, Fn fn)
{
    for (const SubtitleCue& c : cues) {
        int start = c.start, end = c.end;
        if (until >= 0) end = std::min(end, until);
        start = std::max(start, offset);
        if (end <= start) continue;
        QStringList lines = c.text.trimmed().split('\n');
        lines.removeIf([](const QString& line) { return line.trimmed().isEmpty(); });
        if (lines.isEmpty()) continue;
        fn(start - offset, end - offset, lines);
    }
}
} // namespace

QByteArray toVtt(const QVector<SubtitleCue>& cues, double fps, int offset, int until)
{
    QString out = QStringLiteral("WEBVTT\n\n");
    forCues(cues, offset, until, [&](int from, int to, const QStringList& lines) {
        out += srtTime(frameToMs(from, fps)).replace(',', '.') + " --> " + srtTime(frameToMs(to, fps)).replace(',', '.') + '\n';
        // "-->" would end the cue's timing line; '<' and '&' start tags/entities in VTT
        for (QString line : lines) {
            line.replace('&', QLatin1String("&amp;")).replace('<', QLatin1String("&lt;")).replace("-->", QLatin1String("--&gt;"));
            out += line + '\n';
        }
        out += '\n';
    });
    return out.toUtf8();
}

QByteArray toFile(const QString& path, const QVector<SubtitleCue>& cues, double fps, int offset, int until)
{
    return QFileInfo(path).suffix().compare(QLatin1String("vtt"), Qt::CaseInsensitive) == 0
               ? toVtt(cues, fps, offset, until)
               : toSrt(cues, fps, offset, until);
}

QVector<TextTrack> textTracks(const Timeline& tl, double fps, int offset, int until)
{
    QVector<TextTrack> out;
    const QVector<SrtFile> files = srtFiles(tl, QStringLiteral("x.srt"));
    for (int i = 0; i < files.size(); ++i) {
        const SubtitleTrack& st = tl.subtitles[files[i].track];
        TextTrack t;
        t.name = st.name;
        t.main = i == 0;
        forCues(st.cues, offset, until, [&](int from, int to, const QStringList& lines) {
            t.cues << TextCue{frameToMs(from, fps), frameToMs(to, fps), lines.join('\n')};
        });
        if (!t.cues.isEmpty()) out << t;
    }
    return out;
}

TitleStyle defaultStyle(QSize format)
{
    TitleStyle s = subtitleBaseStyle();
    if (format.height() <= 0) return s;
    // Portrait: font by the shorter edge so that lines do not get too short
    const double k = std::min(format.width(), format.height()) / 1080.0;
    const double ky = format.height() / 1080.0;
    s.size = std::round(s.size * k);
    s.outlineWidth = std::round(s.outlineWidth * k * 10) / 10;
    s.boxPad = std::round(s.boxPad * k);
    s.posY = std::round(s.posY * ky);
    return s;
}

void place(SubtitleTrack& track, const SubtitleCue& cue)
{
    if (cue.end <= cue.start) return;
    QVector<SubtitleCue> out;
    for (const SubtitleCue& o : track.cues) {
        if (o.end <= cue.start || o.start >= cue.end) {
            out << o;
            continue;
        }
        // A part whose words were all covered goes (word captions: an empty cue would stay as an invisible block)
        bool leftKept = false;
        if (o.start < cue.start) { // partly covered on the left
            SubtitleCue left = o;
            Captions::crop(left, o.start, cue.start);
            if (!Captions::lostAllWords(o, left)) out << left, leftKept = true;
        }
        if (o.end > cue.end) { // partly covered on the right
            SubtitleCue right = o;
            Captions::crop(right, cue.end, o.end);
            if (leftKept) right.id = 0; // split: right part needs a new id (the editor assigns it)
            if (!Captions::lostAllWords(o, right)) out << right;
        }
    }
    out << cue;
    std::sort(out.begin(), out.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    track.cues = out;
}

void move(Timeline& tl, const QVector<int>& ids, int delta, int trackDelta, const std::function<int()>& newId)
{
    QVector<QPair<int, SubtitleCue>> moving; // target track, entry
    QSet<int> movingIds;
    for (int id : ids) {
        int t = -1;
        if (const SubtitleCue* c = find(std::as_const(tl), id, &t); c && !movingIds.contains(id)) {
            moving << qMakePair(t + trackDelta, *c);
            movingIds.insert(id);
        }
    }
    if (moving.isEmpty()) return;
    // all out first, then placed anew: entries of the selection do not overwrite each other
    for (SubtitleTrack& t : tl.subtitles)
        t.cues.erase(std::remove_if(t.cues.begin(), t.cues.end(), [&](const SubtitleCue& c) { return movingIds.contains(c.id); }),
                     t.cues.end());
    for (auto m : moving) {
        m.second.start += delta;
        m.second.end += delta;
        place(tl.subtitles[m.first], m.second);
    }
    for (SubtitleTrack& t : tl.subtitles) // split entries get their own id
        for (SubtitleCue& c : t.cues)
            if (c.id == 0) c.id = newId();
}

const SubtitleCue* find(const Timeline& tl, int id, int* trackIndex)
{
    if (id <= 0) return nullptr;
    for (int t = 0; t < tl.subtitles.size(); ++t)
        for (const SubtitleCue& c : tl.subtitles[t].cues)
            if (c.id == id) {
                if (trackIndex) *trackIndex = t;
                return &c;
            }
    return nullptr;
}

SubtitleCue* find(Timeline& tl, int id, int* trackIndex)
{
    // Not via the const version + const_cast: the QVectors share their data with other copies (undo states),
    // only the non-const access detaches them
    int track = -1;
    const SubtitleCue* c = find(std::as_const(tl), id, &track);
    if (!c) return nullptr;
    if (trackIndex) *trackIndex = track;
    QVector<SubtitleCue>& cues = tl.subtitles[track].cues;
    for (SubtitleCue& x : cues)
        if (x.id == id) return &x;
    return nullptr;
}

int cueAt(const SubtitleTrack& track, int frame)
{
    for (int i = 0; i < track.cues.size(); ++i)
        if (frame >= track.cues[i].start && frame < track.cues[i].end) return i;
    return -1;
}

int endFrame(const Timeline& tl)
{
    int end = 0;
    for (const SubtitleTrack& t : tl.subtitles)
        if (!t.cues.isEmpty()) end = std::max(end, t.cues.last().end);
    return end;
}

int splitAt(SubtitleTrack& track, int frame, const std::function<int()>& newId)
{
    for (int i = 0; i < track.cues.size(); ++i) {
        SubtitleCue& c = track.cues[i];
        if (c.start >= frame || c.end <= frame) continue;
        // Word captions: the words go to the part they are spoken in (plain cues keep the text on both sides)
        SubtitleCue right = c;
        Captions::crop(right, frame, c.end);
        SubtitleCue left = c;
        Captions::crop(left, c.start, frame);
        // A side without any of the words (a cut in a pause after or before them) is left out instead of staying
        // as an empty, invisible cue: the cue just ends (or starts) at the cut
        const bool rightEmpty = Captions::lostAllWords(c, right), leftEmpty = Captions::lostAllWords(c, left);
        if (rightEmpty || leftEmpty) {
            if (!leftEmpty) c = left;
            else if (!rightEmpty) c = right; // keeps its id
            return 0;
        }
        c = left;
        right.id = newId();
        track.cues.insert(i + 1, right);
        return right.id;
    }
    return 0;
}

bool ripple(SubtitleTrack& track, const QVector<QPair<int, int>>& shifts)
{
    if (shifts.isEmpty() || track.cues.isEmpty()) return false;
    // new position of an edge t: subtract removed time before it; positive offsets only count for entries
    // that start entirely after it (an entry across the cut is not stretched). Word captions: each word moves with
    // the time it is spoken in (the words after the cut go along with its clip, the cue grows by them)
    auto pos = [&](int t, int cueStart) {
        int p = t;
        for (const auto& [frame, d] : shifts) {
            if (d > 0 && frame <= cueStart) p += d;
            else if (d < 0) p -= std::clamp(t - (frame + d), 0, -d);
        }
        return std::max(0, p);
    };
    QVector<SubtitleCue> moved;
    bool changed = false;
    for (SubtitleCue c : track.cues) {
        const bool timed = Captions::timed(c);
        // The end of a word caption goes with its last word
        const int endAnchor = timed ? c.start + c.words.last().from : c.start;
        const int s = pos(c.start, c.start), e = pos(c.end, std::max(c.start, endAnchor));
        changed |= s != c.start || e != c.end;
        if (e <= s) continue; // entirely in removed time
        const int cueStart = c.start;
        const SubtitleCue before = c;
        Captions::remap(c, [&](int t) { return timed ? pos(t, std::max(cueStart, t)) : pos(t, cueStart); }, s, e);
        // A word caption whose words were all in the removed time goes (an empty cue stayed as an invisible block)
        if (Captions::lostAllWords(before, c)) {
            changed = true;
            continue;
        }
        moved << c;
    }
    if (!changed) return false;
    for (int j = 1; j < moved.size(); ++j)
        if (moved[j].start < moved[j - 1].end) return false; // only with contradictory offsets
    track.cues = moved;
    return true;
}

void insertGap(SubtitleTrack& track, int frame, int length, const std::function<int()>& newId)
{
    if (length <= 0) return;
    splitAt(track, frame, newId);
    for (SubtitleCue& c : track.cues)
        if (c.start >= frame) {
            c.start += length;
            c.end += length;
        }
}

// ---- Readable cues ----

namespace {
struct Token {
    QString text;
    int from = 0, to = 0; // timeline frames, [from, to)
    bool known = false;   // from the cue's word times
    int cueEnd = 0;       // end of the cue it came from (where it was shown until)
};

bool sentenceEnd(const QString& w)
{
    QString t = w;
    while (!t.isEmpty() && (t.back() == '"' || t.back() == QChar(0x201C) || t.back() == QChar(0x201D) || t.back() == ')'
                            || t.back() == QChar(0x00BB) || t.back() == QChar(0x00AB)))
        t.chop(1);
    return !t.isEmpty() && (t.back() == '.' || t.back() == '?' || t.back() == '!' || t.back() == QChar(0x2026));
}
bool phraseEnd(const QString& w)
{
    return sentenceEnd(w) || w.endsWith(',') || w.endsWith(';') || w.endsWith(':') || w.endsWith(QChar(0x2013))
        || w.endsWith(QChar(0x2014));
}

// Sorted, without overlaps: a cue that starts inside the one before ends that one there; two starting together
// become one (their text one after the other, no word times)
QVector<SubtitleCue> apart(QVector<SubtitleCue> in)
{
    std::stable_sort(in.begin(), in.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    QVector<SubtitleCue> out;
    for (const SubtitleCue& c : in) {
        if (c.end <= c.start || c.text.trimmed().isEmpty()) continue;
        if (!out.isEmpty() && c.start < out.last().end) {
            SubtitleCue& last = out.last();
            if (c.start <= last.start) {
                last.text += ' ' + c.text;
                last.end = std::max(last.end, c.end);
                last.words.clear();
                continue;
            }
            last.end = c.start;
            for (CueWord& w : last.words) // word times past the new end: squeezed into it
                w.to = std::clamp(w.to, 1, last.length()), w.from = std::clamp(w.from, 0, w.to - 1);
        }
        out << c;
    }
    return out;
}

QVector<Token> tokens(const QVector<SubtitleCue>& cues)
{
    QVector<Token> out;
    for (const SubtitleCue& c : apart(cues)) {
        const QStringList words = c.text.simplified().split(' ', Qt::SkipEmptyParts);
        if (words.isEmpty() || c.end <= c.start) continue;
        const bool known = c.words.size() == words.size();
        double total = 0;
        for (const QString& w : words) total += w.size() + 1;
        double at = 0;
        for (int i = 0; i < words.size(); ++i) {
            Token t;
            t.text = words[i];
            t.known = known;
            t.cueEnd = c.end;
            if (known) {
                t.from = c.start + c.words[i].from;
                t.to = std::max(t.from + 1, c.start + c.words[i].to);
            } else { // the cue's time shared by the words' lengths
                const double len = c.length();
                t.from = c.start + int(std::lround(len * at / total));
                at += words[i].size() + 1;
                t.to = std::max(t.from + 1, c.start + int(std::lround(len * at / total)));
            }
            out << t;
        }
    }
    return out;
}
} // namespace

QVector<SubtitleCue> readable(const QVector<SubtitleCue>& cues, double fps, const ReadableRules& r)
{
    if (fps <= 0) fps = 25;
    const QVector<Token> tok = tokens(cues);
    const int n = int(tok.size());
    const int lines = std::max(1, r.lines), lineChars = std::max(8, r.lineChars);
    const double cps = std::max(1.0, r.cps);
    const int pause = int(std::lround(std::max(0.0, r.pause) * fps));
    const int minFrames = std::max(1, int(std::lround(std::max(0.0, r.minSeconds) * fps)));
    // Characters of tokens [a, b) with one space each between
    QVector<int> pre(n + 1, 0);
    for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + int(tok[i].text.size()) + 1;
    const auto chars = [&](int a, int b) { return b > a ? pre[b] - pre[a] - 1 : 0; };
    // Lines greedy filling needs for [a, b) (the fewest possible)
    const auto lineCount = [&](int a, int b) {
        int count = 1;
        for (int k = a, start = a; k < b; ++k)
            if (k > start && chars(start, k + 1) > lineChars) start = k, ++count;
        return count;
    };
    const auto pauseBefore = [&](int k) { return k > 0 && k < n && tok[k].from - tok[k - 1].to > pause; };
    // The time a cue of [a, b) can be shown: up to the next word (after a pause: the pause too, it just lingers)
    const auto room = [&](int a, int b) {
        const int until = b < n ? tok[b].from : std::max(tok[b - 1].to, tok[b - 1].cueEnd);
        return std::max(1, until - tok[a].from);
    };
    const auto slowEnough = [&](int a, int b) {
        const int t = room(a, b);
        return t >= minFrames && chars(a, b) / (t / fps) <= cps;
    };

    QVector<QPair<int, int>> groups;
    for (int a = 0; a < n;) {
        int b = a + 1;
        bool full = false;
        while (b < n) {
            if (sentenceEnd(tok[b - 1].text) && slowEnough(a, b)) break;
            if (pauseBefore(b)) break;
            if (lineCount(a, b + 1) > lines) {
                full = true;
                break;
            }
            ++b;
        }
        if (full) {
            // Ended by its size: rather at the last phrase end that leaves a readable first part of some length
            for (int k = b - 1; k > a; --k)
                if (phraseEnd(tok[k - 1].text) && chars(a, k) >= lineChars / 2 && slowEnough(a, k)) {
                    b = k;
                    break;
                }
        }
        groups << qMakePair(a, b);
        a = b;
    }

    QVector<SubtitleCue> out;
    for (int g = 0; g < groups.size(); ++g) {
        const auto [a, b] = groups[g];
        SubtitleCue c;
        c.start = tok[a].from;
        if (!out.isEmpty()) c.start = std::max(c.start, out.last().end);
        int end = std::max(tok[b - 1].to, tok[b - 1].cueEnd);
        const int needed = std::max(minFrames, int(std::ceil(chars(a, b) / cps * fps)));
        end = std::max(end, c.start + needed);
        if (b < n) end = std::min(end, tok[b].from);
        c.end = std::max(end, c.start + 1);
        // Lines: one when it fits, else the most even split (a phrase end counts as a few characters closer)
        QStringList words;
        for (int k = a; k < b; ++k) words << tok[k].text;
        if (chars(a, b) <= lineChars || lines < 2) {
            c.text = words.join(' ');
        } else if (lineCount(a, b) <= 2) {
            int best = -1;
            double bestCost = 1e18;
            for (int k = a + 1; k < b; ++k) {
                const int l1 = chars(a, k), l2 = chars(k, b);
                if (l1 > lineChars || l2 > lineChars) continue;
                const double cost = std::abs(l1 - l2) - (phraseEnd(tok[k - 1].text) ? 16 : 0);
                if (cost < bestCost) bestCost = cost, best = k;
            }
            c.text = best < 0 ? words.join(' ') // a word longer than a line: the picture wraps it
                              : words.mid(0, best - a).join(' ') + '\n' + words.mid(best - a).join(' ');
        } else { // more lines allowed: filled greedily
            QStringList rows;
            int start = a;
            for (int k = a; k < b; ++k)
                if (k > start && chars(start, k + 1) > lineChars) rows << words.mid(start - a, k - start).join(' '), start = k;
            rows << words.mid(start - a).join(' ');
            c.text = rows.join('\n');
        }
        bool known = true;
        for (int k = a; k < b; ++k) known = known && tok[k].known;
        if (known)
            for (int k = a; k < b; ++k) {
                const int from = std::clamp(tok[k].from - c.start, 0, c.length() - 1);
                c.words << CueWord{from, std::clamp(tok[k].to - c.start, from + 1, c.length())};
            }
        out << c;
    }
    return out;
}

} // namespace Subtitles
