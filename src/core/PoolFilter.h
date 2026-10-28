// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Filter of the Media Pool's clip list (filter button next to the search): by kind, flag and clip color. A group
// matches when the clip has any of the entries ticked there (nothing ticked = no limit); all groups with ticks must
// match. Session state of the Media Pool, not part of the project.

#include "core/Types.h"

#include <QSet>
#include <QString>

struct PoolFilter {
    enum Kind : unsigned {
        Video = 1u << 0,    // files with moving pictures
        Audio = 1u << 1,    // files with sound only
        Image = 1u << 2,    // still images
        Timeline = 1u << 3, // timelines, compound and multicam clips
    };
    static constexpr unsigned kAllKinds = Video | Audio | Image | Timeline;
    // Clip color id for "no clip color" (the kTrackColors ids never take it)
    static constexpr const char* NoColor = "none";

    unsigned kinds = 0;   // Kind bits; 0 = every kind
    QSet<QString> flags;  // ids from kFlagColors
    QSet<QString> colors; // ids from kTrackColors, NoColor = clips without a clip color

    bool active() const { return kinds != 0 || !flags.isEmpty() || !colors.isEmpty(); }
    bool matches(const MediaInfo& m) const;
    // Timelines have no flags and no clip color: shown only while no flag or color is ticked
    bool matches(const Sequence& s) const;
    static Kind kindOf(const MediaInfo& m);
    // What is filtered, short and translated ("Video, Audio · Flag: Red · Color: Blue"); empty = nothing
    QString summary() const;
    bool operator==(const PoolFilter&) const = default;
};
