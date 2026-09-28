#pragma once
#include <QString>

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

} // namespace Timecode
