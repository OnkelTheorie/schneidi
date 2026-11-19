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
    QVector<Pick> out;
    QVector<bool> used(shots.size(), false);
    for (const Slot& slot : slotList) {
        Pick p;
        p.slot = slot.id;
        const auto free = [&](int i) { return !used[i] && shots[i].video && shots[i].length() > 0; };
        const auto longEnough = [&](int i) { return slot.minS <= 0 || shots[i].length() + 1e-6 >= slot.minS; };
        // What the slot asks for, against the shots still free
        const QString query = (slot.label + QLatin1Char(' ') + slot.hint).trimmed();
        QHash<QString, const IndexSearch::Hit*> hits;
        QVector<IndexSearch::Hit> found;
        if (!query.isEmpty()) {
            QVector<IndexSearch::Shot> search;
            for (int i = 0; i < shots.size(); ++i)
                if (free(i)) search << shots[i].shot;
            found = IndexSearch::search(query, search, 0);
            for (const IndexSearch::Hit& h : std::as_const(found)) hits.insert(key(h.file, h.n), &h);
        }
        int best = -1;
        double bestScore = -1e9;
        bool bestMatched = false;
        for (int i = 0; i < shots.size(); ++i) {
            if (!free(i) || !longEnough(i)) continue;
            const IndexSearch::Hit* h = hits.value(key(shots[i].shot.file, shots[i].shot.n));
            const double score = (h ? kMatchWeight * h->score : 0.0) + quality(shots[i]);
            // Without index data and without a match the material's order decides (the first free one)
            if (!h && !measured(shots[i]) && best >= 0) continue;
            if (score > bestScore) {
                best = i;
                bestScore = score;
                bestMatched = h != nullptr;
            }
        }
        if (best < 0) {
            // Nothing long enough: the longest one left (the fill trims or fits it)
            for (int i = 0; i < shots.size(); ++i)
                if (free(i) && (best < 0 || shots[i].length() > shots[best].length())) best = i;
            p.shortShot = best >= 0;
            if (best >= 0) bestScore = quality(shots[best]);
        }
        if (best >= 0) {
            used[best] = true;
            p.shot = best;
            p.score = std::round(bestScore * 100) / 100;
            quality(shots[best], &p.quality);
            if (const IndexSearch::Hit* h = hits.value(key(shots[best].shot.file, shots[best].shot.n))) {
                for (const IndexSearch::Reason& r : h->reasons)
                    if (!p.matched.contains(r.term)) p.matched << r.term;
            }
            p.inOrder = !bestMatched && !measured(shots[best]);
            QStringList why;
            if (!p.matched.isEmpty()) why << QString("matches %1").arg(p.matched.join(", "));
            if (!p.quality.isEmpty()) why << p.quality.join(", ");
            if (p.inOrder) why << "next in the material (no index data)";
            if (p.shortShot) why << QString("the longest shot left (%1 s, the slot wants %2 s)")
                                        .arg(std::round(shots[best].length() * 10) / 10)
                                        .arg(std::round(slot.minS * 10) / 10);
            if (why.isEmpty()) why << "the best shot left";
            p.why = why.join("; ");
        } else {
            p.why = "no shot left";
        }
        out << p;
    }
    return out;
}

} // namespace SlotPick
