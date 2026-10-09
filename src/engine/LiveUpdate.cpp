#include "engine/LiveUpdate.h"

#include "engine/ColorGrade.h"

#include <Mlt.h>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

namespace LiveUpdate {

namespace {

// Values transfer() carries: copied from `from` so that only structural differences remain
void takeValues(Clip& c, const Clip& from)
{
    c.transform = from.transform;
    c.keys = from.keys;
    c.volumeDb = from.volumeDb;
    c.pan = from.pan;
    c.fadeIn = from.fadeIn;
    c.fadeOut = from.fadeOut;
    if (c.effects.size() == from.effects.size())
        for (int i = 0; i < c.effects.size(); ++i)
            if (c.effects[i].effectId == from.effects[i].effectId) c.effects[i].params = from.effects[i].params;
}

bool sameCut(Timeline after, const Timeline& before)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        QVector<Track>& a = after.tracks(k);
        const QVector<Track>& b = before.tracks(k);
        if (a.size() != b.size()) return false;
        for (int t = 0; t < a.size(); ++t) {
            if (a[t].clips.size() != b[t].clips.size()) return false;
            for (int i = 0; i < a[t].clips.size(); ++i) takeValues(a[t].clips[i], b[t].clips[i]);
        }
    }
    return after == before;
}

bool isUnderscore(const char* name) { return name && name[0] == '_'; }

struct Walk {
    std::vector<std::pair<mlt_properties, mlt_properties>> copies; // live <- fresh
    std::vector<std::pair<mlt_filter, mlt_filter>> filterPairs; // own filters with state (color grade)
    std::set<std::pair<void*, void*>> seen;

    // Every public text property of `fresh` equal in `live` (leaf producers: file, title text, color …)
    static bool sameProperties(mlt_properties live, mlt_properties fresh)
    {
        for (int i = 0, n = mlt_properties_count(fresh); i < n; ++i) {
            const char* name = mlt_properties_get_name(fresh, i);
            if (!name || isUnderscore(name)) continue;
            const char* v = mlt_properties_get_value(fresh, i);
            if (!v) continue; // data
            const char* w = mlt_properties_get(live, name);
            if (!w || std::strcmp(v, w) != 0) return false;
        }
        return true;
    }

    static bool sameText(mlt_properties a, mlt_properties b, const char* name)
    {
        const char* x = mlt_properties_get(a, name);
        const char* y = mlt_properties_get(b, name);
        return (!x && !y) || (x && y && std::strcmp(x, y) == 0);
    }

    bool filters(mlt_service a, mlt_service b)
    {
        const int n = mlt_service_filter_count(a);
        if (n != mlt_service_filter_count(b)) return false;
        for (int i = 0; i < n; ++i) {
            mlt_filter fa = mlt_service_filter(a, i), fb = mlt_service_filter(b, i);
            if (!fa || !fb || fa->process != fb->process) return false;
            if (!sameText(MLT_FILTER_PROPERTIES(fa), MLT_FILTER_PROPERTIES(fb), "mlt_service")) return false;
            if (fa == fb) continue;
            filterPairs.emplace_back(fa, fb);
            copies.emplace_back(MLT_FILTER_PROPERTIES(fa), MLT_FILTER_PROPERTIES(fb));
        }
        return true;
    }

    // Transitions and filters planted in a tractor's field, down to the multitrack
    bool field(mlt_tractor a, mlt_tractor b)
    {
        // the tractor reads the top of the chain (the last planted transition/filter, or the multitrack)
        mlt_service sa = mlt_service_producer(MLT_TRACTOR_SERVICE(a)), sb = mlt_service_producer(MLT_TRACTOR_SERVICE(b));
        const mlt_service ma = MLT_MULTITRACK_SERVICE(mlt_tractor_multitrack(a));
        const mlt_service mb = MLT_MULTITRACK_SERVICE(mlt_tractor_multitrack(b));
        while (sa != ma || sb != mb) {
            if (!sa || !sb || sa == ma || sb == mb) return false;
            const mlt_service_type ta = mlt_service_identify(sa);
            if (ta != mlt_service_identify(sb) || (ta != mlt_service_transition_type && ta != mlt_service_filter_type))
                return false;
            mlt_properties pa = MLT_SERVICE_PROPERTIES(sa), pb = MLT_SERVICE_PROPERTIES(sb);
            if (!sameText(pa, pb, "mlt_service")) return false;
            if (ta == mlt_service_filter_type
                && reinterpret_cast<mlt_filter>(sa)->process != reinterpret_cast<mlt_filter>(sb)->process)
                return false;
            if (sa != sb) copies.emplace_back(pa, pb);
            sa = mlt_service_producer(sa);
            sb = mlt_service_producer(sb);
        }
        return true;
    }

    bool producer(mlt_producer a, mlt_producer b)
    {
        if (a == b) return true;
        if (!a || !b) return false;
        if (!seen.insert({a, b}).second) return true;
        mlt_service sa = MLT_PRODUCER_SERVICE(a), sb = MLT_PRODUCER_SERVICE(b);
        const mlt_service_type t = mlt_service_identify(sa);
        if (t != mlt_service_identify(sb)) return false;
        mlt_properties pa = MLT_PRODUCER_PROPERTIES(a), pb = MLT_PRODUCER_PROPERTIES(b);
        if (!sameText(pa, pb, "mlt_service") || !sameText(pa, pb, "resource")) return false;
        if (mlt_producer_get_in(a) != mlt_producer_get_in(b) || mlt_producer_get_out(a) != mlt_producer_get_out(b))
            return false;
        if (!filters(sa, sb)) return false;
        if (mlt_producer_is_cut(a) != mlt_producer_is_cut(b)) return false;
        if (mlt_producer_is_cut(a)) {
            copies.emplace_back(pa, pb);
            return producer(mlt_producer_cut_parent(a), mlt_producer_cut_parent(b));
        }
        switch (t) {
        case mlt_service_tractor_type: {
            auto ta = reinterpret_cast<mlt_tractor>(a), tb = reinterpret_cast<mlt_tractor>(b);
            return field(ta, tb)
                   && producer(MLT_MULTITRACK_PRODUCER(mlt_tractor_multitrack(ta)),
                               MLT_MULTITRACK_PRODUCER(mlt_tractor_multitrack(tb)));
        }
        case mlt_service_multitrack_type: {
            auto ma = reinterpret_cast<mlt_multitrack>(a), mb = reinterpret_cast<mlt_multitrack>(b);
            const int n = mlt_multitrack_count(ma);
            if (n != mlt_multitrack_count(mb)) return false;
            for (int i = 0; i < n; ++i)
                if (!producer(mlt_multitrack_track(ma, i), mlt_multitrack_track(mb, i))) return false;
            return true;
        }
        case mlt_service_playlist_type: {
            auto la = reinterpret_cast<mlt_playlist>(a), lb = reinterpret_cast<mlt_playlist>(b);
            const int n = mlt_playlist_count(la);
            if (n != mlt_playlist_count(lb)) return false;
            for (int i = 0; i < n; ++i) {
                mlt_playlist_clip_info ia, ib;
                if (mlt_playlist_get_clip_info(la, &ia, i) || mlt_playlist_get_clip_info(lb, &ib, i)) return false;
                if (ia.frame_in != ib.frame_in || ia.frame_out != ib.frame_out || ia.start != ib.start
                    || mlt_playlist_is_blank(la, i) != mlt_playlist_is_blank(lb, i))
                    return false;
                if (!mlt_playlist_is_blank(la, i) && !producer(ia.cut, ib.cut)) return false;
            }
            return true;
        }
        default: // leaf (file, title, color, our ramp producer): must be the same in every visible property
            return sameProperties(pa, pb);
        }
    }

    void apply()
    {
        for (auto [live, fresh] : copies) {
            for (int i = 0, n = mlt_properties_count(fresh); i < n; ++i) {
                const char* name = mlt_properties_get_name(fresh, i);
                if (!name || isUnderscore(name)) continue;
                const char* v = mlt_properties_get_value(fresh, i);
                if (!v) continue;
                const char* w = mlt_properties_get(live, name);
                if (!w || std::strcmp(v, w) != 0) mlt_properties_set(live, name, v);
            }
        }
        for (auto [live, fresh] : filterPairs) ColorGrade::transfer(live, fresh); // no-op for other filters
    }
};

} // namespace

bool valuesOnly(const Timeline& before, const Timeline& after)
{
    if (!sameCut(after, before)) return false;
    if (!before.nested || !after.nested) return !before.nested && !after.nested;
    if (before.nested->size() != after.nested->size()) return false;
    for (auto it = before.nested->begin(); it != before.nested->end(); ++it) {
        auto other = after.nested->find(it.key());
        if (other == after.nested->end() || !sameCut(*other, *it)) return false;
    }
    return true;
}

bool transfer(Mlt::Service& live, Mlt::Service& fresh)
{
    if (live.type() != mlt_service_tractor_type || fresh.type() != mlt_service_tractor_type) return false;
    Walk w;
    if (!w.producer(reinterpret_cast<mlt_producer>(live.get_service()), reinterpret_cast<mlt_producer>(fresh.get_service())))
        return false;
    w.apply();
    return true;
}

} // namespace LiveUpdate
