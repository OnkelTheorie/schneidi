#include "core/Subtitles.h"

#include <QRegularExpression>
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
        if (o.start < cue.start) { // partly covered on the left
            SubtitleCue left = o;
            left.end = cue.start;
            out << left;
        }
        if (o.end > cue.end) { // partly covered on the right
            SubtitleCue right = o;
            right.start = cue.end;
            if (o.start < cue.start) right.id = 0; // split: right part needs a new id (the editor assigns it)
            out << right;
        }
    }
    out << cue;
    std::sort(out.begin(), out.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
    track.cues = out;
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
        SubtitleCue right = c;
        right.id = newId();
        right.start = frame;
        c.end = frame;
        track.cues.insert(i + 1, right);
        return right.id;
    }
    return 0;
}

bool ripple(SubtitleTrack& track, const QVector<QPair<int, int>>& shifts)
{
    if (shifts.isEmpty() || track.cues.isEmpty()) return false;
    // new position of an edge t: subtract removed time before it; positive offsets only count for entries
    // that start entirely after it (an entry across the cut is not stretched)
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
        const int s = pos(c.start, c.start), e = pos(c.end, c.start);
        changed |= s != c.start || e != c.end;
        if (e <= s) continue; // entirely in removed time
        c.start = s;
        c.end = e;
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

} // namespace Subtitles
