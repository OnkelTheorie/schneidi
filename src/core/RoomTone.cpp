// SPDX-License-Identifier: AGPL-3.0-only
#include "core/RoomTone.h"

#include <algorithm>
#include <cmath>

namespace RoomTone {

Quiet findQuiet(const std::vector<double>& blockDb, int want, int least)
{
    Quiet q;
    std::vector<double> sound;
    for (double d : blockDb)
        if (d > kSilentDb) sound.push_back(d);
    if (sound.empty()) return q;
    const size_t k = sound.size() / 10;
    std::nth_element(sound.begin(), sound.begin() + k, sound.end());
    q.floorDb = sound[k];
    const double ceiling = q.floorDb + 6;
    const int n = int(blockDb.size());
    least = std::max(1, least);
    for (int len = std::max(want, least); len >= least; len = len > least ? std::max(least, len * 3 / 4) : 0) {
        double bestMean = 1e9;
        int best = -1;
        for (int i = 0; i + len <= n; ++i) {
            double hiDb = -1e9, loDb = 1e9, power = 0;
            bool ok = true;
            for (int j = i; j < i + len && ok; ++j) {
                if (blockDb[j] <= kSilentDb || blockDb[j] > ceiling) ok = false;
                hiDb = std::max(hiDb, blockDb[j]);
                loDb = std::min(loDb, blockDb[j]);
                power += std::pow(10.0, blockDb[j] / 10);
            }
            if (!ok || hiDb - loDb > kSteadyDb) continue;
            const double mean = 10 * std::log10(power / len);
            if (mean < bestMean) {
                bestMean = mean;
                best = i;
            }
        }
        if (best >= 0) {
            q.from = best;
            q.to = best + len;
            q.meanDb = bestMean;
            return q;
        }
        if (len == least) break;
    }
    return q;
}

int seamFrames(double fps)
{
    return std::max(1, int(std::lround(fps * 0.02)));
}

QVector<int> fill(Timeline& tl, int track, const Fill& f, int fileLength, const std::function<int()>& newId)
{
    QVector<int> ids;
    if (track < 0 || track >= tl.audio.size() || f.to <= f.from || f.quietLength <= 0) return ids;
    auto& clips = tl.audio[track].clips;
    const int seam = std::max(1, f.seam);

    Clip piece = f.take; // file, stream, pan and the take's audio effects (it should sound like the take)
    piece.linkId = 0;
    piece.subframe = 0;
    piece.keys.clear();
    piece.transIn = piece.transOut = 0;
    piece.transInAlone = piece.transOutAlone = false;
    piece.transInStyle = piece.transOutStyle = TransitionStyle{};
    piece.fadeIn = piece.fadeOut = 0;
    piece.speed = 1.0;
    piece.reverse = piece.freeze = false;
    piece.freezeFrame = -1;
    piece.ramp.clear();
    piece.renderCache = false;
    piece.keepRegion.clear();
    piece.enabled = true;
    piece.volumeDb = std::clamp(f.take.volumeDb + f.gainDb, kMinVolumeDb, kMaxVolumeDb);

    // Pieces leave `seam` frames of the quiet stretch after their end: the crossfade into the next piece plays them
    const int step = std::max(1, f.quietLength - seam);
    QVector<Clip> pieces;
    for (int at = f.from; at < f.to; at += step) {
        Clip p = piece;
        p.id = newId();
        p.start = at;
        p.in = f.quietIn;
        p.out = f.quietIn + std::min(step, f.to - at) - 1;
        pieces << p;
    }
    TransitionStyle onCut; // starts on the cut: the left side plays on, the right side from its first frame
    onCut.align = TransitionAlign::Start;
    for (int i = 0; i + 1 < pieces.size(); ++i) {
        pieces[i].transOut = pieces[i + 1].transIn = seam;
        pieces[i].transOutStyle = pieces[i + 1].transInStyle = onCut;
    }

    // The gap's edges: the take fades into the room tone where its edge is free, else the piece fades
    const auto freeMedia = [&](const Clip& c) {
        return c.kind == ClipKind::Media && !c.isRetimed() && c.mediaPath == f.take.mediaPath
               && c.audioStream == f.take.audioStream;
    };
    Clip* prev = nullptr;
    Clip* next = nullptr;
    for (Clip& c : clips) {
        if (c.end() == f.from) prev = &c;
        if (c.start == f.to) next = &c;
    }
    if (prev && freeMedia(*prev) && prev->transOut == 0 && prev->fadeOut == 0 && fileLength - 1 - prev->out >= seam) {
        prev->transOut = pieces.first().transIn = seam;
        prev->transOutStyle = pieces.first().transInStyle = onCut;
        prev->transOutAlone = false;
    } else {
        pieces.first().fadeIn = std::min(seam, pieces.first().length());
    }
    TransitionStyle beforeCut; // ends on the cut: the next take is untouched from its first frame on
    beforeCut.align = TransitionAlign::End;
    if (next && freeMedia(*next) && next->transIn == 0 && next->fadeIn == 0 && next->in >= seam) {
        pieces.last().transOut = next->transIn = seam;
        pieces.last().transOutStyle = next->transInStyle = beforeCut;
        next->transInAlone = false;
    } else {
        pieces.last().fadeOut = std::min(seam, pieces.last().length() - pieces.last().fadeIn);
    }
    for (const Clip& p : pieces) {
        clips << p;
        ids << p.id;
    }
    std::sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return ids;
}

} // namespace RoomTone
