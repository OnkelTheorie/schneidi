// Multicam grid for the viewer (docs/archiv/plan-0.8.md): one tractor that plays all angles of a page side by side, in
// sync (frame = frame of the multicam sequence), independent of the timeline, so cutting does not restart playback.
#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "engine/TimelineBuilder.h"
#include "engine/TimelineBuilderDetail.h"
#include "ui/MulticamGrid.h"

#include <Mlt.h>
#include <algorithm>

std::unique_ptr<Mlt::Tractor> TimelineBuilder::buildMulticamGrid(const Timeline& content,
                                                                 const QVector<MulticamAngle>& angles,
                                                                 const MulticamGridOptions& o, MulticamGridHooks* hooks)
{
    if (hooks) *hooks = {};
    if (angles.isEmpty()) return nullptr;
    m_used.clear();
    m_preroll.clear();
    m_nestedCache.clear();
    m_nested = content.nested;

    const int n = angles.size();
    const int per = MulticamGrid::perPage(n, o.columns);
    const int page = std::clamp(o.page, 0, MulticamGrid::pages(n, o.columns) - 1);
    const int end = std::max(1, TimelineOps::endFrame(content)) + 3600 * qRound(m_profile.fps());
    const QRect frame(0, 0, m_profile.width(), m_profile.height());

    auto tractor = std::make_unique<Mlt::Tractor>(m_profile);
    Mlt::Playlist background(m_profile);
    Mlt::Producer black(m_profile, "color:black"); // empty cells and the bars around other aspect ratios
    black.set("length", end);
    background.append(black, 0, end - 1);
    tractor->set_track(background, 0);
    int track = 1;

    // An angle's part as its own nested tractor (own decoders per angle and part) on one playlist track
    auto part = [&](int angle, TrackKind kind) -> std::unique_ptr<Mlt::Playlist> {
        const Timeline t = Multicam::angleTimeline(content, angles[angle], kind);
        if (t.tracks(kind).isEmpty()) return nullptr;
        const QString key = QString("g%1%2/").arg(kind == TrackKind::Video ? "v" : "a").arg(angle);
        std::unique_ptr<Mlt::Tractor> inner = buildInner(t, m_keyPrefix + key);
        if (!inner || !inner->is_valid()) return nullptr;
        auto pl = std::make_unique<Mlt::Playlist>(m_profile);
        pl->append(*inner, 0, end - 1);
        return pl;
    };

    // Picture: the angles of the page, each scaled into its cell (cells have the frame's aspect ratio, the angle's
    // own bars come along -> its aspect ratio is kept)
    for (int i = 0; i < per; ++i) {
        const int angle = page * per + i;
        if (angle >= n) break;
        std::unique_ptr<Mlt::Playlist> pl = part(angle, TrackKind::Video);
        if (!pl) continue; // sound-only angle: its cell stays black
        const QRect cell = MulticamGrid::cell(frame, n, i, o.columns);
        Mlt::Filter place(m_profile, "qtblend");
        if (place.is_valid()) {
            place.set("rect", mlt_rect{double(cell.x()), double(cell.y()), double(cell.width()), double(cell.height()), 1.0});
            place.set("distort", 1);
            pl->attach(place);
            // after qtblend (its get_image runs first): decode/scale the angle near cell size
            TimelineBuilderDetail::attachSourceCap(*pl, 1.1 * cell.width() / frame.width());
        }
        pl->set("hide", 2); // no audio
        tractor->set_track(*pl, track);
        Mlt::Transition t(m_profile, "qtblend");
        t.set("always_active", 1);
        TimelineBuilderDetail::plantBlend(*tractor, t, track);
        ++track;
    }

    // Sound: every angle (also those of other pages), only the active one audible
    std::vector<std::shared_ptr<Mlt::Playlist>> sound(static_cast<size_t>(n));
    for (int angle = 0; angle < n; ++angle) {
        std::unique_ptr<Mlt::Playlist> pl = part(angle, TrackKind::Audio);
        if (!pl) continue;
        pl->set("hide", angle == o.activeAngle ? 1 : 3); // 1 = no video, 2 = no audio
        tractor->set_track(*pl, track);
        Mlt::Transition t(m_profile, "mix");
        t.set("always_active", 1);
        t.set("sum", 1);
        tractor->plant_transition(t, 0, track);
        sound[size_t(angle)] = std::make_shared<Mlt::Playlist>(*pl);
        ++track;
    }
    if (hooks) {
        hooks->audio = std::move(sound);
        hooks->active = std::clamp(o.activeAngle, -1, n - 1);
    }

    m_nested.reset();
    m_nestedCache.clear();
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = m_used.count(it->first) ? std::next(it) : m_cache.erase(it);
    return tractor;
}

bool TimelineBuilder::applyMulticamAudio(MulticamGridHooks& hooks, int angle)
{
    if (angle < 0 || angle >= int(hooks.audio.size())) return false;
    for (size_t i = 0; i < hooks.audio.size(); ++i)
        if (hooks.audio[i]) hooks.audio[i]->set("hide", int(i) == angle ? 1 : 3);
    hooks.active = angle;
    return true;
}
