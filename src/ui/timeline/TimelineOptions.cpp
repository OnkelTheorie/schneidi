// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/timeline/TimelineOptions.h"

#include "core/EditDefaults.h"
#include "core/I18n.h"
#include "core/Timecode.h"

#include <QRegularExpression>
#include <QSettings>
#include <QString>
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

bool spaceReturns() { return stored("timeline/spaceStops") == "return"; }

void setSpaceReturns(bool on) { store("timeline/spaceStops", "return", !on); }

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

TimeFormat timeFormat()
{
    const QString v = stored("timeline/timeFormat");
    return v == "seconds" ? TimeFormat::Seconds : v == "frames" ? TimeFormat::Frames : TimeFormat::Timecode;
}

void setTimeFormat(TimeFormat format)
{
    store("timeline/timeFormat", format == TimeFormat::Seconds ? "seconds" : "frames", format == TimeFormat::Timecode);
}

QString formatTime(int frames, int fps, TimeFormat format)
{
    if (fps <= 0) fps = 25;
    switch (format) {
    case TimeFormat::Frames: return QString::number(frames);
    case TimeFormat::Seconds: {
        const bool neg = frames < 0;
        const long long f = std::llabs((long long)frames);
        const long long sec = f / fps;
        const int hundredths = int((f % fps) * 100 / fps);
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
    return Timecode::format(frames, fps);
}

int parseTime(const QString& text, int fps, TimeFormat format)
{
    QString t = text.trimmed();
    if (t.isEmpty() || fps <= 0) return -1;
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
    return Timecode::parse(t, fps);
}

} // namespace TimelineOptions
