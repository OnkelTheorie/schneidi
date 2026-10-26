#include "app/AiSettings.h"

#include <QSettings>

#include <algorithm>

namespace {

const char* indicatorKey(AiSettings::Indicator i)
{
    switch (i) {
    case AiSettings::Indicator::Always: return "always";
    case AiSettings::Indicator::Never: return "never";
    case AiSettings::Indicator::WhileActive: break;
    }
    return "active";
}

} // namespace

QColor AiSettings::defaultColor() { return QColor(0xb4, 0x82, 0xff); }

AiSettings AiSettings::load()
{
    const QSettings s;
    AiSettings a;
    a.liveAllowed = s.value("ai/liveAllowed", a.liveAllowed).toBool();
    a.highlight = s.value("ai/highlight", a.highlight).toBool();
    if (const QColor c(s.value("ai/color").toString()); c.isValid()) a.color = c;
    a.highlightMs = std::clamp(s.value("ai/highlightMs", a.highlightMs).toInt(), kMinHighlightMs, kMaxHighlightMs);
    const QString ind = s.value("ai/indicator").toString();
    for (Indicator i : {Indicator::Always, Indicator::WhileActive, Indicator::Never})
        if (ind == indicatorKey(i)) a.indicator = i;
    return a;
}

void AiSettings::save() const
{
    QSettings s;
    s.setValue("ai/liveAllowed", liveAllowed);
    s.setValue("ai/highlight", highlight);
    s.setValue("ai/color", color.name());
    s.setValue("ai/highlightMs", highlightMs);
    s.setValue("ai/indicator", indicatorKey(indicator));
}
