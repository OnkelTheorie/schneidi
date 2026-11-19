// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SlotPick.h"

#include <QHash>

#include <algorithm>
#include <cmath>

namespace SlotPick {

namespace {

constexpr double kMatchWeight = 10.0; // a match on what the slot asks for beats any quality difference

bool measured(const Shot& s) { return s.sharp >= 0 || s.bright >= 0; }

QString key(const QString& file, int n) { return file + QLatin1Char('#') + QString::number(n); }

} // namespace

double quality(const Shot& s, QStringList* what)
{
    double q = 0;
    const auto say = [what](const char* w) {
        if (what) *what << QString::fromLatin1(w);
    };
    if (s.sharp >= 0) {
        q += 2.0 * std::clamp(s.sharp, 0.0, 1.0);
        if (s.shot.sharp) say("sharp");
    }
    if (s.shot.blurry) {
        q -= 1.5;
        say("blurry");
    }
    if (s.bright >= 0) {
        // Best in the middle; 0 at the edges of the usable range
        q += std::max(0.0, 1.0 - std::abs(s.bright - 0.5) / 0.3);
        if (!s.shot.dark && !s.shot.overexposed && std::abs(s.bright - 0.5) < 0.2) say("well exposed");
    }
    if (s.shot.dark) {
        q -= 1.0;
        say("dark");
    }
    if (s.shot.overexposed) {
        q -= 1.0;
        say("overexposed");
    }
    if (s.faces > 0 || (s.faces < 0 && s.shot.face)) {
        q += 1.0;
        say("face");
    }
    return q;
}

QVector<Pick> pick(const QVector<Slot>& slotList, const QVector<Shot>& shots)
{
    QVector<Pick> out(slotList.size());
    QVector<bool> used(shots.size(), false), done(slotList.size(), false);
    const auto free = [&](int i) { return !used[i] && shots[i].video && shots[i].length() > 0; };
    const auto longEnough = [&](const Slot& slot, int i) { return slot.minS <= 0 || shots[i].length() + 1e-6 >= slot.minS; };
    // What a slot asks for, against the shots still free (file#n -> hit)
    const auto hitsFor = [&](const Slot& slot, QVector<IndexSearch::Hit>* found) {
        QHash<QString, const IndexSearch::Hit*> hits;
        const QString query = (slot.label + QLatin1Char(' ') + slot.hint).trimmed();
        if (query.isEmpty()) return hits;
        QVector<IndexSearch::Shot> search;
        for (int i = 0; i < shots.size(); ++i)
            if (free(i) && longEnough(slot, i)) search << shots[i].shot;
        *found = IndexSearch::search(query, search, 0);
        for (const IndexSearch::Hit& h : std::as_const(*found)) hits.insert(key(h.file, h.n), &h);
        return hits;
    };
    const auto take = [&](int si, int best, double score, const IndexSearch::Hit* hit, bool shortShot) {
        Pick& p = out[si];
        p.slot = slotList[si].id;
        done[si] = true;
        if (best < 0) {
            p.why = "no shot left";
            return;
        }
        used[best] = true;
        p.shot = best;
        p.score = std::round(score * 100) / 100;
        p.shortShot = shortShot;
        quality(shots[best], &p.quality);
        if (hit)
            for (const IndexSearch::Reason& r : hit->reasons)
                if (!p.matched.contains(r.term)) p.matched << r.term;
        p.inOrder = !hit && !measured(shots[best]);
        QStringList why;
        if (!p.matched.isEmpty()) why << QString("matches %1").arg(p.matched.join(", "));
        if (!p.quality.isEmpty()) why << p.quality.join(", ");
        if (p.inOrder) why << "next in the material (no index data)";
        if (p.shortShot) why << QString("the longest shot left (%1 s, the slot wants %2 s)")
                                    .arg(std::round(shots[best].length() * 10) / 10)
                                    .arg(std::round(slotList[si].minS * 10) / 10);
        if (why.isEmpty()) why << "the best shot left";
        p.why = why.join("; ");
    };
    // Pass 1: the slots something matches, the best match first (a vague slot does not take the shot another one asks
    // for by name)
    for (;;) {
        int bestSlot = -1, bestShot = -1;
        double bestScore = -1e9;
        QVector<IndexSearch::Hit> keep;
        for (int si = 0; si < slotList.size(); ++si) {
            if (done[si]) continue;
            QVector<IndexSearch::Hit> found;
            const auto hits = hitsFor(slotList[si], &found);
            for (int i = 0; i < shots.size(); ++i) {
                const IndexSearch::Hit* h = hits.value(key(shots[i].shot.file, shots[i].shot.n));
                if (!h) continue;
                const double score = kMatchWeight * h->score + quality(shots[i]);
                if (score > bestScore) {
                    bestScore = score;
                    bestSlot = si;
                    bestShot = i;
                    keep = found;
                }
            }
        }
        if (bestSlot < 0) break;
        const IndexSearch::Hit* hit = nullptr;
        for (const IndexSearch::Hit& h : std::as_const(keep))
            if (h.file == shots[bestShot].shot.file && h.n == shots[bestShot].shot.n) hit = &h;
        take(bestSlot, bestShot, bestScore, hit, false);
    }
    // Pass 2: the others in their order, by quality (without index data: the material's order)
    for (int si = 0; si < slotList.size(); ++si) {
        if (done[si]) continue;
        const Slot& slot = slotList[si];
        int best = -1;
        double bestScore = -1e9;
        for (int i = 0; i < shots.size(); ++i) {
            if (!free(i) || !longEnough(slot, i)) continue;
            if (!measured(shots[i]) && best >= 0) continue;
            const double score = quality(shots[i]);
            if (score > bestScore) {
                best = i;
                bestScore = score;
            }
        }
        bool shortShot = false;
        if (best < 0) {
            // Nothing long enough: the longest one left (the fill trims or fits it)
            for (int i = 0; i < shots.size(); ++i)
                if (free(i) && (best < 0 || shots[i].length() > shots[best].length())) best = i;
            shortShot = best >= 0;
            if (best >= 0) bestScore = quality(shots[best]);
        }
        take(si, best, bestScore, nullptr, shortShot);
    }
    return out;
}

} // namespace SlotPick
