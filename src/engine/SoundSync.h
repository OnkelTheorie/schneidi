// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Audio sync of files (multicam, schneidi-cli `sync`): decodes one audio stream of each file into an onset envelope
// (core/AudioSync) and finds the offsets between them. Blocks; long files take a while (progress + cancel).

#include "core/AudioSync.h"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <vector>

namespace SoundSync {

// progress(0..1); false = cancel
using Progress = std::function<bool(double)>;

// Envelope of audio stream `stream` (0 = first) of the file; nullopt = no such file/stream, no audio or canceled
std::optional<std::vector<float>> fileEnvelope(const QString& path, int stream, const Progress& progress = {});

struct Input {
    QString path;
    int stream = 0;
};
// Offsets of all files relative to the first one (AudioSync::alignAll); files without usable sound are not found.
// Canceled: empty list.
QVector<AudioSync::Match> align(const QVector<Input>& files, const AudioSync::Options& options = {},
                                const Progress& progress = {});

} // namespace SoundSync
