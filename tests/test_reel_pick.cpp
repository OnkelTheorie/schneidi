// SPDX-License-Identifier: AGPL-3.0-only
// Picking the moments of a highlight video (core/ReelPick): target length, "all", order best/time, spacing per
// recording, cuts on the beat (Beats::snapToGrid); the short's variants without shared moments (ShortPick::variants)
#include "core/Beats.h"
#include "core/ReelPick.h"
#include "core/ShortPick.h"

#include "check.h"

#include <QCoreApplication>

#include <cmath>

using ReelPick::Moment;

namespace {
double sum(const QVector<Moment>& l)
{
    double s = 0;
    for (const Moment& m : l) s += m.to - m.from;
    return s;
}
} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("reel_pick");

    // ---- Beats::snapToGrid: the nearest beat within the limit, others stay; no grid = unchanged
    {
        Beats::Result r;
        r.found = true;
        r.period = 0.5; // 120 BPM
        r.phase = 0.1;
        const std::vector<double> t = Beats::snapToGrid({0.0, 0.62, 1.37, 2.1, -0.3}, r, 0.25);
        CHECK(std::abs(t[0] - 0.1) < 1e-9);  // before the first beat: the grid goes on
        CHECK(std::abs(t[1] - 0.6) < 1e-9);
        CHECK(std::abs(t[2] - 1.6) < 1e-9);  // 0.23 away
        CHECK(std::abs(t[3] - 2.1) < 1e-9);  // already on the beat
        CHECK(std::abs(t[4] + 0.4) < 1e-9);
        const std::vector<double> far = Beats::snapToGrid({1.37}, r, 0.2);
        CHECK(std::abs(far[0] - 1.37) < 1e-9); // farther than the limit: stays
        Beats::Result none;
        CHECK(Beats::snapToGrid({1.23}, none, 1.0).front() == 1.23);
    }

    const QVector<double> durations{60, 60, 60};
    // Three recordings: two strong moments close together in file 0, a medium one in file 1, weak ones in 2
    const QVector<Moment> c{{0, 10, 14, 30}, {0, 13, 15, 25}, {0, 40, 44, 20}, {1, 5, 9, 15}, {2, 20, 22, 5}, {2, 50, 54, 4}};

    // ---- Target length: the best ones up to it, close ones of a recording joined, spacing keeps scenes apart
    {
        ReelPick::Options o;
        o.targetS = 14;
        o.growBeforeS = o.growAfterS = 0; // the pick alone (growing: below)
        const QVector<Moment> p = ReelPick::pick(c, durations, o);
        CHECK(sum(p) <= 14 + 1e-6);
        // 10-14 and 13-15 (grown to 4 s: 12.1-16.1) are one moment with the better score, first
        if (CHECK(!p.isEmpty())) {
            CHECK_EQ(p[0].file, 0);
            CHECK(std::abs(p[0].from - 10) < 1e-6 && std::abs(p[0].to - 16.1) < 1e-6);
        }
        // Best first
        for (int i = 1; i < p.size(); ++i) CHECK(p[i - 1].score >= p[i].score);
    }
    // ---- Short of the target: a moment as strong as a better one near it comes in (a second multi kill a few
    // seconds later, videotest 21.11.); a weaker echo stays out
    {
        ReelPick::Options o;
        o.targetS = 60;
        o.growBeforeS = o.growAfterS = 0;
        const QVector<Moment> twins{{0, 13, 19, 26}, {0, 20.6, 26, 26}, {0, 2, 6, 11}};
        const QVector<Moment> p = ReelPick::pick(twins, {30.0}, o);
        CHECK_EQ(p.size(), 3);
        for (int i = 1; i < p.size(); ++i) CHECK(p[i - 1].score >= p[i].score);
        const QVector<Moment> echo{{0, 13, 19, 26}, {0, 20.6, 26, 12}};
        CHECK_EQ(ReelPick::pick(echo, {30.0}, o).size(), 1);
        o.targetS = 8; // the target reached: no second one
        CHECK_EQ(ReelPick::pick(twins, {30.0}, o).size(), 1);
    }
    // ---- "all": every moment, minus those too close to a better one of the same recording
    {
        ReelPick::Options o;
        o.targetS = 0;
        o.spacingS = 12;
        const QVector<Moment> p = ReelPick::pick(c, durations, o);
        CHECK_EQ(p.size(), 5); // 10-16.1 (joined), 40-44, 5-9, 19.1-23.1 (grown), 50-54
        o.spacingS = 35;       // 40 is 30 s after 10: now too close too, and 50 after 20
        CHECK_EQ(ReelPick::pick(c, durations, o).size(), 3);
    }
    // ---- Short moments grow to the minimum; order by time keeps the recordings' order
    {
        ReelPick::Options o;
        o.targetS = 0;
        o.order = ReelPick::Order::Time;
        const QVector<Moment> p = ReelPick::pick(c, durations, o);
        for (const Moment& m : p) CHECK(m.to - m.from >= o.minMomentS - 1e-6);
        for (int i = 1; i < p.size(); ++i)
            CHECK(p[i - 1].file < p[i].file || (p[i - 1].file == p[i].file && p[i - 1].from < p[i].from));
    }
    // ---- Cuts on the beat: each cut of the result lands on the grid when it is within 0.25 s
    {
        Beats::Result beats;
        beats.found = true;
        beats.period = 0.6;
        beats.phase = 0;
        ReelPick::Options o;
        o.targetS = 0;
        o.beats = beats;
        const QVector<Moment> p = ReelPick::pick(c, durations, o);
        double at = 0;
        int onBeat = 0;
        for (const Moment& m : p) {
            at += m.to - m.from;
            const double k = at / beats.period;
            onBeat += std::abs(k - std::round(k)) * beats.period < 1e-6;
        }
        CHECK_EQ(onBeat, p.size()); // every cut within 0.3 s of a beat at 0.6 s spacing: all on it
        // Without beats the ends stay where they were
        o.beats.reset();
        const QVector<Moment> plain = ReelPick::pick(c, durations, o);
        CHECK(std::abs(plain[0].to - 16.1) < 1e-6);
    }
    // ---- Fewer moments than the target: they grow (up to 6 s before, 4 s after), never past a recording's ends or
    // into each other, just to the target; plenty of moments: nothing grows. (Highlights of 4 kills of 4 s on 60 s:
    // 16 s of moments and switched-off "end of recording" fillers before.)
    {
        const QVector<double> d{30, 30, 30};
        const QVector<Moment> kills{{0, 14, 18, 27}, {1, 1, 5, 26}, {2, 24, 28, 11}, {0, 26, 30, 10}};
        ReelPick::Options o;
        o.targetS = 60;
        const QVector<Moment> p = ReelPick::pick(kills, d, o);
        CHECK_EQ(p.size(), 4);
        // Room: file 0 at 14-18: 6 before, 4 after (gap to 26-30 is 8: half = 4); file 0 at 26-30: 4 before, 0 after;
        // file 1 at 1-5: 1 before, 4 after; file 2 at 24-28: 6 before, 2 after = 27 s of room, 44 s missing: all of it
        CHECK(std::abs(sum(p) - 43) < 1e-6);
        for (const Moment& m : p) CHECK(m.from >= -1e-9 && m.to <= d[m.file] + 1e-9);
        for (int i = 0; i < p.size(); ++i)
            for (int j = i + 1; j < p.size(); ++j)
                CHECK(!(p[i].file == p[j].file && p[i].from < p[j].to - 1e-9 && p[j].from < p[i].to - 1e-9));
        for (const Moment& m : p)
            if (m.file == 0 && m.score == 27) CHECK(std::abs(m.from - 8) < 1e-6 && std::abs(m.to - 22) < 1e-6);
        // Only what is missing: target 24 = 16 s of moments + 8 s, each grows by 8/27 of its room
        o.targetS = 24;
        CHECK(std::abs(sum(ReelPick::pick(kills, d, o)) - 24) < 1e-6);
        // Enough moments for the target, or 'all': unchanged
        o.targetS = 16;
        CHECK(std::abs(sum(ReelPick::pick(kills, d, o)) - 16) < 1e-6);
        o.targetS = 0;
        CHECK(std::abs(sum(ReelPick::pick(kills, d, o)) - 16) < 1e-6);
    }
    // ---- Grown moments of one recording meet halfway; a cut moved onto the beat never runs into the next one
    // (kills at 10-14 and 20-24 s grow to 4-17 and 17-28: A's end at 13 s of the video would snap to 13.2 = 17.2 s
    // in the recording, 0.2 s into B - that footage twice)
    {
        const QVector<double> d{60};
        Beats::Result beats;
        beats.found = true;
        beats.period = 0.6;
        beats.phase = 0;
        ReelPick::Options o;
        o.targetS = 300;
        o.beats = beats;
        const QVector<Moment> p = ReelPick::pick({{0, 10, 14, 30}, {0, 20, 24, 20}}, d, o);
        if (CHECK_EQ(p.size(), 2)) {
            CHECK(std::abs(p[0].from - 4) < 1e-6);
            CHECK(std::abs(p[1].from - 17) < 1e-6);
            if (!CHECK(p[0].to <= p[1].from + 1e-9)) std::printf("       A ends %.3f, B starts %.3f\n", p[0].to, p[1].from);
            CHECK(p[1].to <= 60 + 1e-9);
        }
        // Order by time and the other way round (B snapped first): no overlap either
        o.order = ReelPick::Order::Time;
        const QVector<Moment> t = ReelPick::pick({{0, 10, 14, 20}, {0, 20, 24, 30}}, d, o);
        for (int i = 0; i < t.size(); ++i)
            for (int j = i + 1; j < t.size(); ++j) CHECK(!(t[i].from < t[j].to - 1e-9 && t[j].from < t[i].to - 1e-9));
    }
    // ---- Short variants: the second shares no moment with the first; none left = no third
    {
        ShortPick::Options o;
        o.minS = 5;
        o.maxS = 12;
        const QVector<ShortPick::Moment> cand{{0, 10, 14, 30}, {0, 40, 44, 20}, {1, 5, 9, 15}, {2, 50, 54, 4}};
        const QVector<QVector<ShortPick::Moment>> v = ShortPick::variants(cand, durations, 3, o);
        if (CHECK(v.size() >= 2)) {
            CHECK(v[0] == ShortPick::pick(cand, durations, o)); // the first is the plain pick
            for (const ShortPick::Moment& a : v[0])
                for (const ShortPick::Moment& b : v[1]) CHECK(!(a.file == b.file && a.from < b.to && b.from < a.to));
        }
        const QVector<QVector<ShortPick::Moment>> one = ShortPick::variants({{0, 10, 14, 30}}, durations, 3, o);
        CHECK_EQ(one.size(), 1);
    }
    // ---- A later variant grown to its minimum never reaches into an earlier one's moment
    {
        ShortPick::Options o;
        o.minS = 7;
        o.maxS = 7;
        const QVector<ShortPick::Moment> cand{{0, 10, 12, 30}, {0, 16, 18, 20}};
        const QVector<QVector<ShortPick::Moment>> v = ShortPick::variants(cand, durations, 2, o);
        if (CHECK_EQ(v.size(), 2))
            for (const ShortPick::Moment& a : v[0])
                for (const ShortPick::Moment& b : v[1]) CHECK(!(a.file == b.file && a.from < b.to - 1e-9 && b.from < a.to - 1e-9));
    }
    return Check::result();
}
