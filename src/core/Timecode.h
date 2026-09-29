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

// Eingabe wie in DaVinci -> Frames, -1 = ungültig. "01:02:03:04" (Teile von rechts: Frames, Sekunden, Minuten,
// Stunden; auch mit ; oder .), nur Ziffern füllen von rechts auf ("1000" = 00:00:10:00).
inline int parse(const QString& text, int fps)
{
    QString t = text.trimmed();
    if (t.isEmpty() || fps <= 0) return -1;
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
    const int mult[] = {1, fps, fps * 60, fps * 3600}; // von rechts
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
