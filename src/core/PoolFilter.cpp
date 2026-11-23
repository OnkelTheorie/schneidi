// SPDX-License-Identifier: AGPL-3.0-only
#include "core/PoolFilter.h"

#include "core/I18n.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

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

QStringList poolSearchWords(const MediaInfo& m, QSize picture, double fps)
{
    QStringList w;
    auto both = [&](const char* german, const char* english) {
        w << T(german);
        w << QString::fromLatin1(english);
    };
    switch (PoolFilter::kindOf(m)) {
    case PoolFilter::Image: both(N_("Bild"), "image"), w << "picture" << "photo" << T("Foto"); break;
    case PoolFilter::Video: both(N_("Video"), "video"); break;
    default: both(N_("Audio"), "audio"), w << T("Ton") << "sound"; break;
    }
    if (const TrackColorInfo* c = trackColorInfo(m.clipColor)) w << T(c->name) << QString::fromLatin1(c->id);
    for (const QString& id : m.flags)
        if (const FlagColorInfo* f = flagColorInfo(id)) w << T(f->name) << QString::fromLatin1(f->id) << T("Flag") << "flag";
    if (!m.hdr.isEmpty()) w << "HDR" << m.hdr;
    if (picture.width() > 0 && picture.height() > 0) {
        const int longSide = std::max(picture.width(), picture.height());
        const int shortSide = std::min(picture.width(), picture.height());
        w << QString("%1x%2").arg(picture.width()).arg(picture.height());
        if (shortSide >= 4320 || longSide >= 7680) w << "8K";
        else if (shortSide >= 2160 || longSide >= 3840) w << "4K" << "UHD" << "2160p";
        else if (shortSide >= 1440) w << "2K" << "1440p" << "QHD";
        else if (shortSide >= 1080) w << "1080p" << "Full HD" << "FHD";
        else if (shortSide >= 720) w << "720p" << "HD";
        else w << "SD" << QString("%1p").arg(shortSide);
        const double ratio = double(longSide) / shortSide;
        auto near = [&](double r) { return std::abs(ratio - r) < 0.02; };
        if (picture.height() > picture.width()) {
            both(N_("hochkant"), "vertical"), w << T("Hochformat") << "portrait";
            if (near(16.0 / 9)) w << "9:16";
        } else if (picture.width() > picture.height()) {
            both(N_("quer"), "horizontal"), w << T("Querformat") << "landscape";
            if (near(16.0 / 9)) w << "16:9";
            else if (near(4.0 / 3)) w << "4:3";
            else if (near(21.0 / 9) || near(64.0 / 27)) w << "21:9";
        } else {
            both(N_("quadratisch"), "square"), w << "1:1";
        }
    }
    if (fps > 0) {
        const long whole = std::lround(fps);
        QStringList rates{QString::number(whole)};
        if (std::abs(fps - double(whole)) > 0.005) rates << QString::number(fps, 'f', 2); // 29.97 is "30" and "29.97"
        for (const QString& r : rates) w << r + " fps" << r + "fps" << r + "p";
    }
    return w;
}
