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
    a.releaseMinutes = std::clamp(s.value("ai/releaseMinutes", a.releaseMinutes).toInt(), 0, kMaxReleaseMinutes);
    a.highlight = s.value("ai/highlight", a.highlight).toBool();
    if (const QColor c(s.value("ai/color").toString()); c.isValid()) a.color = c;
    a.highlightMs = std::clamp(s.value("ai/highlightMs", a.highlightMs).toInt(), kMinHighlightMs, kMaxHighlightMs);
    a.distinguish = s.value("ai/distinguish", a.distinguish).toBool();
    a.logLength = std::clamp(s.value("ai/logLength", a.logLength).toInt(), 0, kMaxLogLength);
    const QString ind = s.value("ai/indicator").toString();
    for (Indicator i : {Indicator::Always, Indicator::WhileActive, Indicator::Never})
        if (ind == indicatorKey(i)) a.indicator = i;
    a.chatProvider = s.value("ai/chat/provider").toString();
    a.chatAddress = s.value("ai/chat/address").toString().trimmed();
    const QVariantMap models = s.value("ai/chat/models").toMap();
    for (auto it = models.begin(); it != models.end(); ++it)
        if (const QString m = it.value().toString().trimmed(); !m.isEmpty()) a.chatModels.insert(it.key(), m);
    a.approval = s.value("ai/chat/approval").toString() == "review" ? Approval::Review : Approval::Auto;
    return a;
}

void AiSettings::save() const
{
    QSettings s;
    s.setValue("ai/liveAllowed", liveAllowed);
    s.setValue("ai/releaseMinutes", releaseMinutes);
    s.setValue("ai/highlight", highlight);
    s.setValue("ai/color", color.name());
    s.setValue("ai/highlightMs", highlightMs);
    s.setValue("ai/distinguish", distinguish);
    s.setValue("ai/logLength", logLength);
    s.setValue("ai/indicator", indicatorKey(indicator));
    s.setValue("ai/chat/provider", chatProvider);
    s.setValue("ai/chat/address", chatAddress);
    QVariantMap models;
    for (auto it = chatModels.begin(); it != chatModels.end(); ++it) models.insert(it.key(), it.value());
    s.setValue("ai/chat/models", models);
    s.setValue("ai/chat/approval", approval == Approval::Review ? "review" : "auto");
}
