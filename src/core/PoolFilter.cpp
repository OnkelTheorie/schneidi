// SPDX-License-Identifier: AGPL-3.0-only
#include "core/PoolFilter.h"

#include "core/I18n.h"

#include <QStringList>

#include <algorithm>

PoolFilter::Kind PoolFilter::kindOf(const MediaInfo& m)
{
    if (m.isImage) return Image;
    return m.hasVideo ? Video : Audio;
}

bool PoolFilter::matches(const MediaInfo& m) const
{
    if (kinds && !(kinds & kindOf(m))) return false;
    if (!flags.isEmpty() && std::none_of(m.flags.cbegin(), m.flags.cend(), [&](const QString& f) { return flags.contains(f); }))
        return false;
    if (!colors.isEmpty() && !colors.contains(m.clipColor.isEmpty() ? QString::fromLatin1(NoColor) : m.clipColor))
        return false;
    return true;
}

bool PoolFilter::matches(const Sequence&) const
{
    return (!kinds || (kinds & Timeline)) && flags.isEmpty() && colors.isEmpty();
}

QString PoolFilter::summary() const
{
    QStringList parts;
    QStringList names;
    const struct { Kind kind; const char* name; } kindNames[] = {
        {Video, N_("Video")}, {Audio, N_("Audio")}, {Image, N_("Standbilder")}, {Timeline, N_("Timelines")}};
    for (const auto& k : kindNames)
        if (kinds & k.kind) names << T(k.name);
    if (!names.isEmpty()) parts << names.join(", ");
    names.clear();
    for (const auto& f : kFlagColors) // in the order of the flag menu
        if (flags.contains(QLatin1String(f.id))) names << T(f.name);
    if (!names.isEmpty()) parts << T("Flag: %1").arg(names.join(", "));
    names.clear();
    if (colors.contains(QLatin1String(NoColor))) names << T("ohne");
    for (const auto& c : kTrackColors)
        if (colors.contains(QLatin1String(c.id))) names << T(c.name);
    if (!names.isEmpty()) parts << T("Clipfarbe: %1").arg(names.join(", "));
    return parts.join(" · ");
}
