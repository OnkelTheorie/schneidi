// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Voiceover.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace Voiceover {

Settings Settings::load()
{
    QSettings s;
    Settings out;
    out.input = s.value("audio/input").toString();
    out.offsetMs = std::clamp(s.value("voiceover/offsetMs", 0).toInt(), -2000, 2000);
    out.prerollSeconds = std::clamp(s.value("voiceover/preroll", 0).toInt(), 0, 10);
    out.muteTrack = s.value("voiceover/muteTrack", false).toBool();
    return out;
}

void Settings::save() const
{
    QSettings s;
    s.setValue("audio/input", input);
    s.setValue("voiceover/offsetMs", offsetMs);
    s.setValue("voiceover/preroll", prerollSeconds);
    s.setValue("voiceover/muteTrack", muteTrack);
}

QString folder(const QString& projectPath)
{
    if (projectPath.isEmpty())
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("voiceover");
    const QFileInfo fi(projectPath);
    return fi.absoluteDir().filePath(fi.completeBaseName() + "-voiceover");
}

QString uniquePath(const QString& dir, const QDateTime& when)
{
    const QString base = "voiceover-" + when.toString("yyyyMMdd-HHmmss");
    const QDir d(dir);
    QString path = d.filePath(base + ".wav");
    for (int n = 2; QFileInfo::exists(path); ++n) path = d.filePath(QString("%1-%2.wav").arg(base).arg(n));
    return path;
}

std::optional<Placement> place(double fileStart, int from, int fileFrames)
{
    // First whole file frame whose timeline position is at or after `from`; within half a subframe counts as on it
    constexpr double tolerance = 0.005;
    const int in = std::max(0, int(std::ceil(double(from) - fileStart - tolerance)));
    if (in >= fileFrames) return std::nullopt;
    const double at = fileStart + in;
    Placement p;
    p.in = in;
    p.start = int(std::floor(at + tolerance));
    p.subframe = std::clamp(int(std::lround((at - p.start) * 100)), 0, 99);
    if (p.start < 0) return std::nullopt; // only with from < 0
    return p;
}

void Alignment::add(long long capturedFrames, double heardSeconds, int rate)
{
    if (rate <= 0 || !std::isfinite(heardSeconds)) return;
    // A standing clock (the player waits for the picture, a hiccup) says nothing about when the sound was heard:
    // only pairs taken while it runs count
    const bool running = m_lastHeard && heardSeconds > *m_lastHeard;
    m_lastHeard = heardSeconds;
    if (!running) return;
    const double v = heardSeconds - double(capturedFrames) / rate;
    if (!m_best || v < *m_best) m_best = v;
}

std::optional<double> Alignment::fileStartSeconds(int inputLatency, int rate, int offsetMs) const
{
    if (!m_best || rate <= 0) return std::nullopt;
    return *m_best - double(inputLatency) / rate - offsetMs / 1000.0;
}

} // namespace Voiceover
