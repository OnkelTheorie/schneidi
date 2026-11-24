// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/timeline/TimelineOptions.h"

#include "core/EditDefaults.h"
#include "core/I18n.h"
#include "core/Timecode.h"

#include <QRegularExpression>
#include <QSettings>
#include <QString>
#include <array>
#include <cmath>

namespace TimelineOptions {

namespace {

// A choice stored by name; the default is not stored (a later change of the default reaches it)
void store(const char* key, const char* value, bool isDefault)
{
    if (isDefault) QSettings().remove(key);
    else QSettings().setValue(key, value);
}

QString stored(const char* key) { return QSettings().value(key).toString(); }

} // namespace

Follow follow()
{
    const QString v = stored("timeline/followPlayback");
    return v == "page" ? Follow::Page : v == "center" ? Follow::Center : Follow::Off;
}

void setFollow(Follow follow)
{
    store("timeline/followPlayback", follow == Follow::Page ? "page" : "center", follow == Follow::Off);
}

bool clickSetsPlayhead() { return QSettings().value("timeline/clickSetsPlayhead").toBool(); }

void setClickSetsPlayhead(bool on) { store("timeline/clickSetsPlayhead", "true", !on); }

bool middleDragPans() { return stored("timeline/middleDrag") != "off"; }

void setMiddleDragPans(bool on) { store("timeline/middleDrag", "off", on); }

bool trimOverwrites() { return stored("timeline/edgeMeetsClip") != "stop"; }

void setTrimOverwrites(bool on) { store("timeline/edgeMeetsClip", "stop", on); }

ClipFormat clipFormat()
{
    const QString v = stored("timeline/firstClipFormat");
    return v == "adapt" ? ClipFormat::Adapt : v == "keep" ? ClipFormat::Keep : ClipFormat::Ask;
}

void setClipFormat(ClipFormat choice)
{
    store("timeline/firstClipFormat", choice == ClipFormat::Adapt ? "adapt" : "keep", choice == ClipFormat::Ask);
}

ShuttleSteps shuttleSteps() { return stored("timeline/shuttleSteps") == "gentle" ? ShuttleSteps::Gentle : ShuttleSteps::Double; }

void setShuttleSteps(ShuttleSteps steps) { store("timeline/shuttleSteps", "gentle", steps == ShuttleSteps::Double); }

double shuttleSpeed(double current, int direction, ShuttleSteps steps)
{
    const int dir = direction < 0 ? -1 : 1;
    const double s = current * dir; // the speed in the pressed direction
    if (s < 1.0) return dir;        // paused, the other way or slower: 1x
    if (steps == ShuttleSteps::Double) return dir * std::min(s * 2, 16.0);
    for (double level : {2.0, 3.0, 5.0, 8.0})
        if (level > s + 1e-9) return dir * level;
    return dir * 8.0;
}

bool spaceReturns() { return stored("timeline/spaceStops") == "return"; }

void setSpaceReturns(bool on) { store("timeline/spaceStops", "return", !on); }

bool scrubAudio() { return stored("timeline/scrubAudio") != "off"; }

void setScrubAudio(bool on) { store("timeline/scrubAudio", "off", on); }

bool snapping() { return QSettings().value("timeline/snapping", true).toBool(); }

void setSnapping(bool on) { store("timeline/snapping", "false", on); }

int snapTargets() { return QSettings().value("timeline/snapTargets", kDefaultSnapTargets).toInt(); }

void setSnapTargets(int targets)
{
    if (targets == kDefaultSnapTargets) QSettings().remove("timeline/snapTargets");
    else QSettings().setValue("timeline/snapTargets", targets);
}

SnapReach snapReach()
{
    const QString v = stored("timeline/snapReach");
    return v == "small" ? SnapReach::Small : v == "large" ? SnapReach::Large : SnapReach::Medium;
}

void setSnapReach(SnapReach reach)
{
    store("timeline/snapReach", reach == SnapReach::Small ? "small" : "large", reach == SnapReach::Medium);
}

int snapPixels(SnapReach reach) { return reach == SnapReach::Small ? 4 : reach == SnapReach::Large ? 14 : 8; }

namespace {
double seconds(const char* key, double def)
{
    const double v = QSettings().value(key, def).toDouble();
    return v > 0 ? v : def;
}
void storeSeconds(const char* key, double v, double def)
{
    if (std::abs(v - def) < 1e-9) QSettings().remove(key);
    else QSettings().setValue(key, v);
}
} // namespace

double stillSeconds() { return seconds("timeline/stillSeconds", EditDefaults::kStillSeconds); }
void setStillSeconds(double v) { storeSeconds("timeline/stillSeconds", v, EditDefaults::kStillSeconds); }
double transitionSeconds() { return seconds("timeline/transitionSeconds", EditDefaults::kTransitionSeconds); }
void setTransitionSeconds(double v) { storeSeconds("timeline/transitionSeconds", v, EditDefaults::kTransitionSeconds); }

void applyEditDefaults()
{
    EditDefaults::setStillSeconds(stillSeconds());
    EditDefaults::setTransitionSeconds(transitionSeconds());
}

namespace {
// "1s", "10f"
Step readStep(const char* key, Step def)
{
    const QString v = stored(key);
    bool ok = false;
    const int n = v.left(v.size() - 1).toInt(&ok);
    if (!ok || n < 1 || n > 100000 || !(v.endsWith('s') || v.endsWith('f'))) return def;
    return Step{n, v.endsWith('s')};
}
void storeStep(const char* key, Step step, Step def)
{
    if (step == def || step.count < 1) QSettings().remove(key);
    else QSettings().setValue(key, QString::number(step.count) + (step.seconds ? 's' : 'f'));
}
} // namespace

Step bigStep() { return readStep("timeline/bigStep", kDefaultBigStep); }
void setBigStep(Step step) { storeStep("timeline/bigStep", step, kDefaultBigStep); }
Step bigNudge() { return readStep("timeline/bigNudge", kDefaultBigNudge); }
void setBigNudge(Step step) { storeStep("timeline/bigNudge", step, kDefaultBigNudge); }

QString stepActionText(StepAction action, Step step)
{
    const int n = step.count;
    const bool one = n == 1;
    switch (action) {
    case StepAction::Back:
        if (step.seconds) return one ? T("1 Sekunde zurück") : T("%1 Sekunden zurück").arg(n);
        return one ? T("1 Frame zurück") : T("%1 Frames zurück").arg(n);
    case StepAction::Forward:
        if (step.seconds) return one ? T("1 Sekunde vor") : T("%1 Sekunden vor").arg(n);
        return one ? T("1 Frame vor") : T("%1 Frames vor").arg(n);
    case StepAction::NudgeLeft:
        if (step.seconds) return one ? T("1 Sekunde nach links schieben") : T("%1 Sekunden nach links schieben").arg(n);
        if (n == 5) return T("5 Frames nach links schieben"); // the default: the text it always had
        return one ? T("1 Frame nach links schieben") : T("%1 Frames nach links schieben").arg(n);
    case StepAction::NudgeRight:
        if (step.seconds) return one ? T("1 Sekunde nach rechts schieben") : T("%1 Sekunden nach rechts schieben").arg(n);
        if (n == 5) return T("5 Frames nach rechts schieben");
        return one ? T("1 Frame nach rechts schieben") : T("%1 Frames nach rechts schieben").arg(n);
    }
    return {};
}

Thumbnails thumbnails()
{
    const QString v = stored("timeline/thumbnails");
    return v == "start" ? Thumbnails::Start : v == "startEnd" ? Thumbnails::StartEnd : v == "none" ? Thumbnails::None
                                                                                                  : Thumbnails::Filmstrip;
}

void setThumbnails(Thumbnails t)
{
    store("timeline/thumbnails", t == Thumbnails::Start ? "start" : t == Thumbnails::StartEnd ? "startEnd" : "none",
          t == Thumbnails::Filmstrip);
}

WaveShape waveShape() { return stored("timeline/waveShape") == "bottom" ? WaveShape::Bottom : WaveShape::Mirrored; }
void setWaveShape(WaveShape shape) { store("timeline/waveShape", "bottom", shape == WaveShape::Mirrored); }
WaveScale waveScale() { return stored("timeline/waveScale") == "linear" ? WaveScale::Linear : WaveScale::Db; }
void setWaveScale(WaveScale scale) { store("timeline/waveScale", "linear", scale == WaveScale::Db); }

double waveHeight(int peak, double gainDb, WaveScale scale)
{
    if (peak <= 0) return 0;
    peak = std::min(peak, 255);
    if (scale == WaveScale::Linear) return std::min(1.0, peak / 255.0 * std::pow(10.0, gainDb / 20.0));
    // -48 dB .. 0 dB, otherwise normal material looks almost flat
    static const auto dbTable = [] {
        std::array<double, 256> t{};
        for (int i = 1; i < 256; ++i) t[i] = std::max(0.0, 1.0 + 20.0 * std::log10(i / 255.0) / 48.0);
        return t;
    }();
    return std::min(1.0, dbTable[peak] + gainDb / 48.0);
}

QLine waveLine(int x, double v, const QRect& body, WaveShape shape, double inset)
{
    if (shape == WaveShape::Bottom) {
        const int bottom = int(body.bottom() - inset);
        const int h = std::max(1, int(v * (body.height() - 2 * inset)));
        return QLine(x, bottom - h, x, bottom);
    }
    const int mid = body.top() + body.height() / 2;
    const int h = std::max(1, int(v * (body.height() / 2.0 - inset)));
    return QLine(x, mid - h, x, mid + h);
}

TimeFormat timeFormat()
{
    const QString v = stored("timeline/timeFormat");
    return v == "seconds" ? TimeFormat::Seconds : v == "frames" ? TimeFormat::Frames : TimeFormat::Timecode;
}

void setTimeFormat(TimeFormat format)
{
    store("timeline/timeFormat", format == TimeFormat::Seconds ? "seconds" : "frames", format == TimeFormat::Timecode);
}

QString formatTime(int frames, double fps, TimeFormat format)
{
    if (!(fps > 0)) fps = 25;
    const int timebase = std::max(1, int(std::lround(fps))); // timecode: whole frames per second (29.97 -> 30)
    switch (format) {
    case TimeFormat::Frames: return QString::number(frames);
    case TimeFormat::Seconds: {
        const bool neg = frames < 0;
        // Real seconds at the real rate, to the nearest hundredth (carried into the seconds)
        const long long total = std::llround(std::llabs((long long)frames) * 100.0 / fps);
        const long long sec = total / 100;
        const int hundredths = int(total % 100);
        const QString rest = QString("%1%2%3")
                                 .arg(sec % 60, 2, 10, QChar('0'))
                                 .arg(I18n::decimalPoint())
                                 .arg(hundredths, 2, 10, QChar('0'));
        const QString head = sec >= 3600 ? QString("%1:%2:").arg(sec / 3600).arg((sec / 60) % 60, 2, 10, QChar('0'))
                                         : QString("%1:").arg(sec / 60);
        return (neg ? "-" : "") + head + rest;
    }
    case TimeFormat::Timecode: break;
    }
    return Timecode::format(frames, timebase);
}

int parseTime(const QString& text, double fps, TimeFormat format)
{
    QString t = text.trimmed();
    if (t.isEmpty() || !(fps > 0)) return -1;
    const int timebase = std::max(1, int(std::lround(fps)));
    // "112f": frames in any format
    static const QRegularExpression framesRe(R"(^(\d+)\s*f$)", QRegularExpression::CaseInsensitiveOption);
    if (const auto m = framesRe.match(t); m.hasMatch()) {
        bool ok = false;
        const long long v = m.captured(1).toLongLong(&ok);
        return ok && v <= 2000000000LL ? int(v) : -1;
    }
    if (format == TimeFormat::Frames) {
        static const QRegularExpression digits(R"(^\d+$)");
        if (digits.match(t).hasMatch()) {
            bool ok = false;
            const long long v = t.toLongLong(&ok);
            return ok && v <= 2000000000LL ? int(v) : -1;
        }
    }
    // Seconds: "[h:][m:]s[.cc]" ("4.5s" in any format; with Seconds also without the s)
    static const QRegularExpression secondsRe(R"(^(?:(\d+):)?(?:(\d+):)?(\d+)(?:[.,](\d*))?\s*(s)?$)",
                                              QRegularExpression::CaseInsensitiveOption);
    if (const auto m = secondsRe.match(t); m.hasMatch() && (format == TimeFormat::Seconds || !m.captured(5).isEmpty())) {
        // With one colon the first part is minutes; with two: hours, minutes
        const bool two = !m.captured(2).isEmpty();
        const double hours = two ? m.captured(1).toDouble() : 0;
        const double minutes = two ? m.captured(2).toDouble() : m.captured(1).toDouble();
        const double seconds = (m.captured(3) + '.' + (m.captured(4).isEmpty() ? QString("0") : m.captured(4))).toDouble();
        const double frames = std::round((hours * 3600 + minutes * 60 + seconds) * fps);
        return frames > 2000000000.0 ? -1 : int(frames);
    }
    return Timecode::parse(t, timebase);
}

} // namespace TimelineOptions
