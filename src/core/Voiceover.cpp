// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Voiceover.h"

#include "core/I18n.h"
#include "core/Types.h"

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
    return out;
}

void Settings::save() const
{
    QSettings s;
    s.setValue("audio/input", input);
    s.setValue("voiceover/offsetMs", offsetMs);
    s.setValue("voiceover/preroll", prerollSeconds);
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

int selectedTrack(const Timeline& tl, const QVector<int>& ids)
{
    int track = -1;
    for (int i = 0; i < tl.audio.size(); ++i)
        for (const Clip& c : tl.audio[i].clips)
            if (ids.contains(c.id)) {
                if (track >= 0 && track != i) return -1; // clips on several tracks: none of them is "the" track
                track = i;
            }
    return track;
}

bool isVoiceoverTrack(const Track& t)
{
    return t.name == "Voiceover" || t.name == T("Voiceover");
}

int recordTrack(const Timeline& tl, int armed, int selected, int from, int end)
{
    const int count = int(tl.audio.size());
    if (armed >= 0 && armed < count) return tl.audio[armed].locked ? -1 : armed;
    end = std::max(end, from + 1);
    const auto open = [&](int i) {
        const Track& t = tl.audio[i];
        return !t.locked && !t.muted && (t.name.isEmpty() || isVoiceoverTrack(t));
    };
    const auto freeFrom = [&](int i) {
        for (const Clip& c : tl.audio[i].clips)
            if (c.start < end && c.end() > from) return false;
        return true;
    };
    if (selected >= 0 && selected < count && open(selected) && freeFrom(selected)) return selected;
    for (int i = 0; i < count; ++i)
        if (open(i) && isVoiceoverTrack(tl.audio[i])) return i; // its earlier takes in the range are replaced
    for (int i = 0; i < count; ++i)
        if (open(i) && freeFrom(i)) return i;
    return count;
}

int buttStart(const Timeline& tl, int track, int from, int tolerance)
{
    if (track < 0 || track >= tl.audio.size()) return from;
    int best = from, distance = tolerance + 1;
    for (const Clip& c : tl.audio[track].clips) {
        const int d = std::abs(c.end() - from);
        if (d < distance && c.start < from) { // a clip that ends near the playhead (not one starting there)
            distance = d;
            best = c.end();
        }
    }
    return best;
}

LiveTake::LiveTake(double fileStart, int from, double fps, int rate)
    : m_fileStart(fileStart), m_fps(fps > 0 ? fps : 25), m_from(from), m_rate(std::max(1, rate))
{
}

void LiveTake::add(long long totalFrames, float peak)
{
    if (totalFrames <= m_samples) return;
    const auto bucket = [this](long long sample) { return int(sample * kPeaksPerSecond / m_rate); };
    const int first = bucket(m_samples), last = bucket(totalFrames - 1);
    if (m_peaks.size() <= last) m_peaks.resize(last + 1, 0.f);
    peak = std::clamp(std::isfinite(peak) ? peak : 0.f, 0.f, 1.f);
    for (int i = first; i <= last; ++i) m_peaks[i] = std::max(m_peaks[i], peak);
    m_samples = totalFrames;
}

void LiveTake::addSamples(const float* samples, int count)
{
    if (count <= 0) return;
    float p = 0.f;
    for (int i = 0; i < count; ++i) p = std::max(p, std::abs(samples[i]));
    add(m_samples + count, p);
}

double LiveTake::end() const
{
    return m_fileStart + double(m_samples) / m_rate * m_fps;
}

double LiveTake::lengthFrames() const
{
    return std::max(0.0, end() - m_from);
}

float LiveTake::peak(double a, double b) const
{
    // Timeline frames -> buckets of the file
    const double perFrame = double(kPeaksPerSecond) / m_fps;
    const int i0 = std::max(0, int(std::floor((a - m_fileStart) * perFrame)));
    const int i1 = std::min(int(m_peaks.size()), int(std::ceil((b - m_fileStart) * perFrame)));
    float p = 0.f;
    for (int i = i0; i < i1; ++i) p = std::max(p, m_peaks[i]);
    return p;
}

void Alignment::add(long long capturedFrames, double heardSeconds, int rate)
{
    if (m_ended || rate <= 0 || !std::isfinite(heardSeconds)) return;
    if (m_best && m_lastHeard && heardSeconds < *m_lastHeard) {
        m_ended = true;
        return;
    }
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
