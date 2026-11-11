// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QString>
#include <QStringList>

namespace Timecode {

// Frames -> "HH:MM:SS:FF"
inline QString format(int frames, int fps)
{
    const bool neg = frames < 0;
    if (neg) frames = -frames;
    const int ff = frames % fps;
    const int totalSec = frames / fps;
    return QString("%1%2:%3:%4:%5")
        .arg(neg ? "-" : "")
        .arg(totalSec / 3600, 2, 10, QChar('0'))
        .arg((totalSec / 60) % 60, 2, 10, QChar('0'))
        .arg(totalSec % 60, 2, 10, QChar('0'))
        .arg(ff, 2, 10, QChar('0'));
}

// SMPTE drop-frame timecode exists for 29.97 and 59.94 fps (30000/1001, 60000/1001): "HH:MM:SS;FF"
inline bool isDropFrameRate(int num, int den) { return den == 1001 && (num == 30000 || num == 60000); }

// Frame number of a drop-frame label at `timebase` 30 or 60: the labels ;00 and ;01 (;00-;03 at 60) are skipped at
// the start of every minute except each tenth, so the count keeps up with real time (1 h = 107892 frames at 29.97,
// non-drop would say 108000 - 3.6 s too much). -1 = a label that does not exist (00:01:00;00) or out of range.
inline long long dropFrameNumber(long long h, long long m, long long s, long long f, int timebase)
{
    if (timebase != 30 && timebase != 60) return -1;
    const int drop = timebase / 15; // 2 or 4
    if (h < 0 || m < 0 || m >= 60 || s < 0 || s >= 60 || f < 0 || f >= timebase) return -1;
    if (s == 0 && f < drop && m % 10 != 0) return -1;
    const long long minutes = h * 60 + m;
    return ((h * 3600 + m * 60 + s) * timebase + f) - drop * (minutes - minutes / 10);
}

// Frames of one hour of timecode: 3600 * timebase, or drop-frame (107892 at 29.97, 215784 at 59.94)
inline int hourFrames(int timebase, bool dropFrame)
{
    return dropFrame && (timebase == 30 || timebase == 60) ? int(dropFrameNumber(1, 0, 0, 0, timebase)) : 3600 * timebase;
}

// Input as in DaVinci -> frames, -1 = invalid. "01:02:03:04" (parts from the right: frames, seconds, minutes,
// hours; also with ; or .), digits only fill from the right ("1000" = 00:00:10:00).
// dropFrame (a 29.97/59.94 source, isDropFrameRate): a full "HH:MM:SS;FF" is read as drop-frame timecode - what
// cameras, ffprobe and EDLs of such material write. Without it ';' is only a separator (what the UI shows is
// non-drop "HH:MM:SS:FF").
inline int parse(const QString& text, int fps, bool dropFrame = false)
{
    QString t = text.trimmed();
    if (t.isEmpty() || fps <= 0) return -1;
    if (dropFrame && t.count(';') == 1 && t.count(':') == 2 && t.indexOf(';') > t.lastIndexOf(':')) {
        const QStringList p = QString(t).replace(';', ':').split(':');
        long long v[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            v[i] = p[i].toLongLong(&ok);
            if (!ok || p[i].isEmpty()) return -1;
        }
        const long long frames = dropFrameNumber(v[0], v[1], v[2], v[3], fps);
        return frames < 0 || frames > 2000000000LL ? -1 : int(frames);
    }
    QStringList parts;
    if (t.contains(':') || t.contains(';') || t.contains('.')) {
        t.replace(';', ':').replace('.', ':');
        parts = t.split(':');
    } else {
        for (const QChar ch : t)
            if (!ch.isDigit()) return -1;
        t = t.rightJustified(8, '0');
        const int n = int(t.size());
        parts = {t.left(n - 6), t.mid(n - 6, 2), t.mid(n - 4, 2), t.right(2)};
    }
    if (parts.size() > 4) return -1;
    const int mult[] = {1, fps, fps * 60, fps * 3600}; // from the right
    long long frames = 0;
    for (int i = 0; i < parts.size(); ++i) {
        bool ok = false;
        const QString part = parts[parts.size() - 1 - i];
        const int v = part.isEmpty() ? 0 : part.toInt(&ok);
        if (!part.isEmpty() && (!ok || v < 0)) return -1;
        frames += (long long)v * mult[i];
    }
    return frames > 2000000000LL ? -1 : int(frames);
}

} // namespace Timecode
