// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Hearable events of a recording (speech, music, silence, laughter, applause, cheering, shot, scream).
// PLACEHOLDER of package C (plan 0.17, fixed interface between A and C): returns nothing until C's
// engine/AudioEvents is merged - take C's files over these on merge.

#include <QString>
#include <QVector>

namespace AudioEvents {

struct Event {
    QString kind;            // speech, music, silence, laughter, applause, cheering, shot, scream
    double from = 0, to = 0; // seconds in the file
    double conf = 0;         // 0..1
};

struct Options {};

QVector<Event> analyze(const QString& path, int stream, const Options& options = {});

} // namespace AudioEvents
