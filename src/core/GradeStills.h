// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Stills of the Color page: a small picture of a graded frame together with the grade it had, kept in the project.
// Compared against the live picture in the viewer (a wipe) and their grade applied to other clips - to match shots.

#include "core/Types.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>

struct GradeStill {
    int id = 0;
    QString label;    // clip name and timecode
    QByteArray image; // JPEG, at most kMaxWidth wide (the project file stays small)
    Clip grade;       // the source clip with only its grade (effect "grade" and the keyframes of its values)
    bool operator==(const GradeStill&) const = default;
};

namespace GradeStills {

inline constexpr int kMaxWidth = 640;
inline constexpr int kMaxCount = 64; // per project; the oldest goes when a new one comes past this

// A still of `picture` (the frame as shown) with the grade of `source`
GradeStill make(int id, const QString& label, const QImage& picture, const Clip& source);
QImage picture(const GradeStill& s); // decoded (null if broken)
int nextId(const QVector<GradeStill>& stills);

} // namespace GradeStills
