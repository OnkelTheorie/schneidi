// SPDX-License-Identifier: AGPL-3.0-only
#include "core/TemplateFill.h"

#include "core/MediaAspect.h"
#include "core/MusicFit.h"
#include "core/ProjectFormat.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>

namespace TemplateFill {
namespace {

constexpr double kMinFillSpeed = 0.25; // SlotFit::Speed slows down to at most a quarter
constexpr double kMinShrink = 0.4;     // a shrunk text keeps at least 40 % of its size
constexpr double kMusicFadeOut = 1.0;  // seconds, a song trimmed without its beats

QString secs(double frames, double fps) { return QString::number(frames / fps, 'f', 1) + "s"; }
double round3(double v) { return std::round(v * 1000) / 1000; }

QSizeF frameOf(const ProjectData& d, int sequence)
{
    for (const Sequence& s : d.sequences)
        if (s.id == sequence && s.hasOwnSize()) return QSizeF(s.width, s.height);
    return QSizeF(d.format.width, d.format.height);
}

// The open timeline first, then the other sequences
QVector<QPair<int, const Timeline*>> timelines(const ProjectData& d)
{
    QVector<QPair<int, const Timeline*>> out{{d.currentSequence, &d.timeline}};
    for (const Sequence& s : d.sequences)
        if (s.id != d.currentSequence) out.append({s.id, &s.timeline});
    return out;
}

// The timeline to edit, with the project's frame rate (comp values in seconds follow its cuts, Comp::followHead)
Timeline* timelineOf(ProjectData& d, int sequence)
{
    Timeline* tl = nullptr;
    if (sequence == d.currentSequence) tl = &d.timeline;
    for (Sequence& s : d.sequences)
        if (!tl && s.id == sequence) tl = &s.timeline;
    if (tl) tl->frameRate = d.format.rate.fps();
    return tl;
}

std::optional<MediaInfo> mediaOf(const ProjectData& d, const Context& ctx, const QString& path)
{
    for (const MediaInfo& m : d.media)
        if (m.path == path) return m;
    if (ctx.probe) return ctx.probe(path);
    return std::nullopt;
}

double aspectOf(const Context& ctx, const QString& path)
{
    return ctx.aspect ? ctx.aspect(path) : MediaAspect::of(path);
}
double clipAspect(const Context& ctx, const Clip& c) { return c.pictureAspect > 0 ? c.pictureAspect : aspectOf(ctx, c.mediaPath); }

int textChars(const QString& text) { return int(text.size() - text.count('\n')); }

// The part of the file a value plays: source frames [from, from + length); length -1 = a still (any length)
struct Range {
    int from = 0, length = 0;
};
Range rangeOf(const MediaInfo& m, const Value& v)
{
    if (m.isImage) return {0, -1};
    const bool marks = v.from < 0 && v.to < 0; // the Media Pool's source in/out only when the value gives neither
    const int from = v.from >= 0 ? v.from : marks && m.markIn >= 0 ? m.markIn : 0;
    int end = v.to >= 0 ? v.to : marks && m.markOut >= 0 ? m.markOut + 1 : m.length;
    if (m.length > 0) end = std::min(end, m.length);
    return {from, end - from};
}

Problem problem(const QString& slot, const char* code, const QString& message)
{
    return {slot, QString::fromLatin1(code), message};
}

std::optional<Problem> checkOne(const ProjectData& d, const SlotRef& r, const Value& v, const Context& ctx)
{
    const Timeline* tl = nullptr;
    for (const auto& [seq, t] : timelines(d))
        if (seq == r.sequence) tl = t;
    const Clip* c = tl ? TimelineOps::findClip(*tl, r.clip) : nullptr;
    if (!c) return problem(r.slot.id, "SLOT_MISMATCH", "the slot's clip is gone");
    if (const QString why = canHold(*c, r.track.kind, r.slot.kind); !why.isEmpty())
        return problem(r.slot.id, "SLOT_MISMATCH", why);
    if (tl->track(r.track).locked)
        return problem(r.slot.id, "LOCKED", QString("the slot's track %1 is locked").arg(trackShortName(r.track)));
    if (r.slot.kind == SlotKind::Text) {
        if (v.text.trimmed().isEmpty()) return problem(r.slot.id, "EMPTY_TEXT", "give the slot's text");
        const int n = textChars(v.text);
        if (r.slot.maxChars > 0 && n > r.slot.maxChars && !r.slot.shrink)
            return problem(r.slot.id, "TOO_LONG", QString("too long: %1/%2 chars").arg(n).arg(r.slot.maxChars));
        return std::nullopt;
    }
    if (v.file.isEmpty()) return problem(r.slot.id, "MISSING_VALUE", "give a file for the slot");
    const std::optional<MediaInfo> m = mediaOf(d, ctx, v.file);
    if (!m) {
        if (!QFileInfo::exists(v.file)) return problem(r.slot.id, "FILE_MISSING", "file not found: " + v.file);
        return problem(r.slot.id, "UNREADABLE", "cannot read the file: " + v.file);
    }
    if (const QString why = canFill(r.slot.kind, *m); !why.isEmpty())
        return problem(r.slot.id, "WRONG_TYPE", why + ": " + v.file);
    if (!m->isImage) {
        if (m->length <= 0) return problem(r.slot.id, "UNREADABLE", "the file's length is unknown: " + v.file);
        const Range rg = rangeOf(*m, v);
        if (rg.from < 0 || rg.from >= m->length || rg.length <= 0)
            return problem(r.slot.id, "BAD_RANGE",
                           QString("from/to outside the file (it has %1 frames): %2").arg(m->length).arg(v.file));
    }
    return std::nullopt;
}

// Keyframes count in material frames from the clip's source (like `in`): moved along with a new `in`
void shiftKeys(Clip& c, int delta)
{
    if (!delta) return;
    for (KeyTrack& k : c.keys)
        for (Keyframe& f : k) f.frame += delta;
}

void scaleKeys(Clip& c, AnimParam p, double f)
{
    if (!c.keys.contains(p)) return;
    for (Keyframe& k : c.keys[p]) k.value *= f;
}

// Everything after `oldEnd` on all unlocked tracks moves by delta (< 0: [oldEnd + delta, oldEnd) is taken out; a clip
// reaching over that range or ending right at oldEnd, a music bed or a title over the slot, gets shorter at its end
// instead of being cut, so its end keeps its fade-out and transition)
void ripple(Timeline& tl, int oldEnd, int delta, const IdGen& newId)
{
    if (!delta) return;
    const int newEnd = oldEnd + delta;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : tl.tracks(k)) {
            if (t.locked) continue;
            if (delta < 0) {
                QVector<Clip> over;
                for (int i = int(t.clips.size()) - 1; i >= 0; --i)
                    if (t.clips[i].start < newEnd && t.clips[i].end() >= oldEnd) over << t.clips.takeAt(i);
                TimelineOps::clearRange(t, newEnd, oldEnd, newId, tl.frameRate);
                TimelineOps::shiftFrom(t, oldEnd, delta);
                for (Clip c : over) {
                    c.out = std::max(c.in, c.out + delta);
                    c.fadeOut = std::min(c.fadeOut, c.length());
                    c.fadeIn = std::min(c.fadeIn, c.length() - c.fadeOut);
                    TimelineOps::placeClip(t, c, newId, tl.frameRate);
                }
            } else {
                TimelineOps::shiftFrom(t, oldEnd, delta);
            }
        }
    for (SubtitleTrack& st : tl.subtitles) {
        if (st.locked) continue;
        QVector<SubtitleCue> cues;
        for (SubtitleCue c : st.cues) {
            if (c.start >= oldEnd) {
                c.start += delta, c.end += delta;
            } else if (delta < 0 && c.end > newEnd) {
                if (c.start >= newEnd && c.end <= oldEnd) continue; // inside what was taken out
                if (c.end > oldEnd) c.end += delta;
                else c.end = newEnd;
                if (c.start >= newEnd) c.start = newEnd;
                c.words.clear(); // their times no longer fit
            }
            if (c.end > c.start) cues << c;
        }
        st.cues = cues;
    }
    const auto moved = [&](int f) { return f >= oldEnd ? f + delta : delta < 0 && f > newEnd ? newEnd : f; };
    for (int& m : tl.markers) m = moved(m);
    tl.markers.erase(std::unique(tl.markers.begin(), tl.markers.end()), tl.markers.end());
    for (TimelineNote& n : tl.notes) n.at = moved(n.at);
    QVector<StorySection> sections;
    for (StorySection s : tl.sections) {
        s.from = moved(s.from);
        s.to = s.to >= oldEnd ? s.to + delta : moved(s.to);
        if (s.to > s.from) sections << s;
    }
    tl.sections = sections;
}

// The clip as the start of the new file's parts: what belonged to the old file goes (ramp, reverse, freeze, kept
// region, its own picture shape), its look and its own constant speed stay (fillMedia: the slot's own one)
Clip prepared(const Clip& base, const MediaInfo& m)
{
    Clip c = base;
    c.mediaPath = m.path;
    c.ramp.clear();
    c.reverse = false;
    c.freeze = false;
    c.freezeFrame = -1;
    c.pictureAspect = m.pictureAspect;
    c.cropOnFrame = false;
    c.keepRegion.clear();
    if (c.audioStream >= m.audioStreamCount()) c.audioStream = 0;
    return c;
}

struct Parts {
    QVector<Clip> clips; // positioned from the slot's start, ids of all but the first 0
    int length = 0;
};

void setText(SlotReport& rep, const QString& s)
{
    rep.text = rep.text.isEmpty() ? s : rep.text + "; " + s;
}

// The parts a file fills a Clip/Music/Logo slot with (rules in the header)
Parts plan(const Clip& base, const SlotRef& r, const MediaInfo& m, const Range& rg, double fps, SlotReport& rep)
{
    const ClipSlot& slot = r.slot;
    Clip c = prepared(base, m);
    const int S = r.slotLength;
    const auto clampLen = [&](int len) {
        if (slot.length != SlotLength::Flexible) return S;
        len = std::max(len, std::max(1, slot.minLen));
        return slot.maxLen > 0 ? std::min(len, slot.maxLen) : len;
    };
    Parts p;
    const auto add = [&](Clip part, int in, int len, bool first, bool last) {
        shiftKeys(part, in - base.in);
        part.in = in;
        part.out = in + len - 1;
        part.start = r.start + p.length;
        if (!first) part.id = 0, part.transIn = 0, part.fadeIn = 0, part.transInAlone = false;
        if (!last) part.transOut = 0, part.fadeOut = 0, part.transOutAlone = false;
        p.clips << part;
        p.length += len;
    };
    if (rg.length < 0) { // a still: any length
        const int T = clampLen(S);
        add(c, 0, T, true, true);
        return p;
    }
    const double sp = c.speed > 0 ? c.speed : 1.0;
    const int mFrom = int(std::ceil(rg.from / sp - 1e-6));
    const int mL = std::max(1, int(std::floor(rg.length / sp + 1e-6)));
    const int T = clampLen(mL);
    rep.details["value_s"] = round3(rg.length / fps);
    rep.details["slot_s"] = round3(S / fps);
    if (mL >= T) {
        add(c, mFrom, T, true, true);
        if (mL > T) {
            rep.adjusted << "trimmed";
            setText(rep, QString("trimmed %1 → %2").arg(secs(rg.length, fps), secs(T * sp, fps)));
        }
        return p;
    }
    SlotFit fit = slot.fit;
    if (slot.kind == SlotKind::Music && (fit == SlotFit::Speed || fit == SlotFit::Freeze)) fit = SlotFit::Trim;
    switch (fit) {
    case SlotFit::Trim:
        add(c, mFrom, mL, true, true);
        rep.adjusted << "shortened";
        setText(rep, QString("shorter than the slot: %1 of %2").arg(secs(mL, fps), secs(T, fps)));
        if (slot.length == SlotLength::Fixed)
            rep.warnings << QString("the value is shorter than the slot (%1 of %2): a gap of %3 stays")
                                .arg(secs(mL, fps), secs(T, fps), secs(T - mL, fps));
        else
            rep.warnings << QString("the value is shorter than the slot's minimum length (%1 of %2)")
                                .arg(secs(mL, fps), secs(T, fps));
        break;
    case SlotFit::Speed: {
        double v = sp * double(mL) / T;
        int len = T;
        if (v < kMinFillSpeed) {
            v = kMinFillSpeed;
            len = int(std::floor(rg.length / v + 1e-6));
            rep.warnings << QString("slowed down to the limit of 25 %: %1 of the slot's %2 filled")
                                .arg(secs(len, fps), secs(T, fps));
        }
        c.speed = v;
        const int in = int(std::ceil(rg.from / v - 1e-6));
        len = std::max(1, std::min(len, int(std::floor((rg.from + rg.length) / v + 1e-6)) - in));
        add(c, in, len, true, true);
        rep.adjusted << "slowed";
        rep.details["speed_pct"] = std::round(v * 1000) / 10;
        setText(rep, QString("slowed to %1 %").arg(QString::number(v * 100, 'f', 0)));
        break;
    }
    case SlotFit::Freeze: {
        add(c, mFrom, mL, true, false);
        Clip still = c;
        still.freeze = true;
        still.freezeFrame = mFrom + mL - 1;
        still.linkId = 0;
        add(still, mFrom + mL, T - mL, false, true);
        rep.adjusted << "frozen";
        rep.details["frozen_s"] = round3((T - mL) / fps);
        setText(rep, QString("last frame held for %1").arg(secs(T - mL, fps)));
        break;
    }
    case SlotFit::Loop: {
        int n = 0;
        while (p.length < T) {
            const int len = std::min(mL, T - p.length);
            add(c, mFrom, len, n == 0, p.length + len >= T);
            ++n;
        }
        rep.adjusted << "looped";
        rep.details["repeats"] = n;
        setText(rep, QString("looped %1×").arg(n));
        break;
    }
    }
    return p;
}

// Clip slot: the picture of its new file in the timeline's shape
void reframePart(Clip& c, const Clip& base, const SlotRef& r, const MediaInfo& m, int sourceFrom, const QSizeF& frame,
                 const Context& ctx, SlotReport& rep)
{
    const double newAspect = c.pictureAspect > 0 ? c.pictureAspect : aspectOf(ctx, m.path);
    if (newAspect <= 0) {
        if (r.slot.reframe && !rep.warnings.contains("picture shape unknown: not reframed"))
            rep.warnings << "picture shape unknown: not reframed";
        return;
    }
    const double oldAspect = clipAspect(ctx, base);
    if (oldAspect > 0 && std::abs(oldAspect - newAspect) > 1e-3 && Reframe::reshape(c, oldAspect, newAspect, frame)) {
        if (!rep.adjusted.contains("reshaped")) rep.adjusted << "reshaped", setText(rep, "reframed for the new shape");
        return;
    }
    const double frameAspect = frame.width() / frame.height();
    if (!r.slot.reframe || std::abs(newAspect - frameAspect) / frameAspect <= 0.02
        || Reframe::isOverlay(c, r.track.index, newAspect, frame))
        return;
    QVector<Reframe::Key> keys;
    if (ctx.reframePath) keys = ctx.reframePath(m.path, sourceFrom, c.length());
    Reframe::apply(c, keys.isEmpty() ? Reframe::Mode::Center : Reframe::Mode::Auto, keys, newAspect, frame);
    if (!rep.adjusted.contains("reframed")) {
        rep.adjusted << "reframed";
        rep.details["reframe"] = keys.isEmpty() ? "center" : "auto";
        setText(rep, keys.isEmpty() ? "reframed (center)" : "reframed (auto)");
    }
}

// Logo slot: the same width on screen with the new picture
void keepWidth(Clip& c, const Clip& base, const MediaInfo& m, const QSizeF& frame, const Context& ctx, SlotReport& rep)
{
    const double oldAspect = clipAspect(ctx, base);
    const double newAspect = c.pictureAspect > 0 ? c.pictureAspect : aspectOf(ctx, m.path);
    if (oldAspect <= 0 || newAspect <= 0) {
        rep.warnings << "picture shape unknown: the logo's size may change";
        return;
    }
    const double f = MediaAspect::fit(oldAspect, frame).width() / MediaAspect::fit(newAspect, frame).width();
    if (std::abs(f - 1) < 1e-6) return;
    c.transform.zoomX *= f;
    c.transform.zoomY *= f;
    scaleKeys(c, AnimParam::ZoomX, f);
    scaleKeys(c, AnimParam::ZoomY, f);
    rep.adjusted << "scaled";
    rep.details["zoom"] = round3(c.transform.zoomX);
    setText(rep, QString("zoom %1 to keep its width").arg(QString::number(c.transform.zoomX, 'f', 2)));
}

void fillTitle(Clip& c, const SlotRef& r, const Value& v, SlotReport& rep)
{
    const TitleStyle old = c.title;
    c.title.text = v.text;
    rep.adjusted << "replaced";
    setText(rep, "text set");
    const int n = textChars(v.text);
    rep.details["chars"] = n;
    if (r.slot.maxChars > 0 && n > r.slot.maxChars)
        rep.warnings << QString("too long: %1/%2 chars").arg(n).arg(r.slot.maxChars);
    if (!r.slot.shrink || old.size <= 0) return;
    // Width is linear in the size (titleWidth): one step
    const double oldWidth = titleWidth(old, old.size), newWidth = titleWidth(c.title, old.size);
    if (newWidth <= oldWidth * 1.001) return;
    double q = oldWidth / newWidth;
    if (q < kMinShrink) {
        q = kMinShrink;
        rep.warnings << "the text is still wider than the placeholder's text at 40 % of its size";
    }
    c.title.size = std::round(old.size * q * 10) / 10;
    scaleKeys(c, AnimParam::TitleSize, q);
    rep.adjusted << "shrunk";
    rep.details["size_px"] = c.title.size;
    setText(rep, QString("text shrunk to %1 px").arg(QString::number(c.title.size, 'g', 4)));
}

// Every part of the slot (a title cut in two) gets the text; each part shrinks from its own size (the report tells the
// first one)
void fillText(Timeline& tl, const SlotRef& r, const Value& v, SlotReport& rep)
{
    for (int i = 0; i < r.parts.size(); ++i) {
        SlotReport once;
        if (Clip* c = TimelineOps::findClip(tl, r.parts[i]); c && c->isTitle()) fillTitle(*c, r, v, i == 0 ? rep : once);
    }
}
// A ripple at `oldEnd` by `delta` inside `sequence`: the compound clips that show it follow. What they showed after
// oldEnd moved by delta, so a clip showing that point gets longer/shorter (and its own timeline ripples in turn, up
// through nested compounds), one showing only what comes after it moves in its source, one before it stays. A linked
// group (a compound clip's picture and sound) follows as a whole: with a part on a locked track or retimed, none of it
// does (a warning). `followed` counts the clips changed.
void followCompounds(ProjectData& d, int sequence, int oldEnd, int delta, const IdGen& newId, SlotReport& rep,
                     int& followed, int depth)
{
    if (!delta || depth > 32) return;
    const int newEnd = oldEnd + delta;
    const auto moved = [&](int f) { return f >= oldEnd ? f + delta : delta < 0 && f > newEnd ? newEnd : f; };
    QVector<int> parents{d.currentSequence};
    for (const Sequence& s : d.sequences)
        if (s.id != d.currentSequence) parents << s.id;
    for (int parent : parents) {
        if (parent == sequence) continue;
        Timeline* tl = timelineOf(d, parent);
        if (!tl) continue;
        struct Change {
            int id, in, out; // new in/out
            int at, dd;      // the parent's frame that shows oldEnd (or the clip's end if it shows only what is before
                             // it) and the clip's change of length
        };
        QVector<Change> changes;
        QSet<int> held; // link ids of groups with a part that stays
        bool stays = false;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : std::as_const(*tl).tracks(k))
                for (const Clip& c : t.clips) {
                    if (!c.isCompound() || c.sequenceId != sequence) continue;
                    const int in = moved(c.in), end = moved(c.out + 1);
                    if (in == c.in && end == c.out + 1) continue;
                    if (t.locked || c.isRetimed()) {
                        stays = true;
                        if (c.linkId) held.insert(c.linkId);
                        continue;
                    }
                    changes << Change{c.id, in, end - 1, c.start + std::min(oldEnd, c.out + 1) - c.in,
                                      end - in - c.length()};
                }
        if (stays) {
            const QString w = "a compound clip that shows the slot is on a locked track or retimed: it (and what is "
                              "linked to it) does not follow";
            if (!rep.warnings.contains(w)) rep.warnings << w;
        }
        changes.erase(std::remove_if(changes.begin(), changes.end(),
                                     [&](const Change& ch) {
                                         const Clip* c = TimelineOps::findClip(*tl, ch.id);
                                         return !c || (c->linkId && held.contains(c->linkId));
                                     }),
                      changes.end());
        // Only moved in its source
        for (const Change& ch : changes)
            if (!ch.dd)
                if (Clip* c = TimelineOps::findClip(*tl, ch.id)) {
                    c->in = ch.in, c->out = ch.out;
                    ++followed;
                }
        // Longer/shorter: the parent ripples where the change shows (from the last such place back: a ripple never
        // moves what is before it). The clips themselves are out meanwhile and come back with their new in/out, their
        // fades and transitions kept (the ripple would cut a shrinking one's end off); clips changing at the same
        // place (picture and linked sound) ripple once.
        changes.erase(std::remove_if(changes.begin(), changes.end(), [](const Change& ch) { return !ch.dd; }),
                      changes.end());
        std::sort(changes.begin(), changes.end(),
                  [](const Change& a, const Change& b) { return a.at != b.at ? a.at > b.at : a.dd < b.dd; });
        for (int i = 0; i < changes.size();) {
            int j = i;
            while (j < changes.size() && changes[j].at == changes[i].at && changes[j].dd == changes[i].dd) ++j;
            struct Out {
                TrackRef ref;
                Clip clip;
                int in, out;
            };
            QVector<Out> out;
            for (int k = i; k < j; ++k) {
                TrackRef ref;
                if (const Clip* c = TimelineOps::findClip(*tl, changes[k].id, &ref)) {
                    out << Out{ref, *c, changes[k].in, changes[k].out};
                    TimelineOps::removeClip(*tl, changes[k].id);
                }
            }
            ripple(*tl, changes[i].at, changes[i].dd, newId);
            for (Out& o : out) {
                ++followed;
                if (o.out < o.in) continue; // all it showed was taken out
                o.clip.in = o.in;
                o.clip.out = o.out;
                TimelineOps::placeClip(tl->track(o.ref), o.clip, newId, tl->frameRate);
            }
            followCompounds(d, parent, changes[i].at, changes[i].dd, newId, rep, followed, depth + 1);
            i = j;
        }
    }
}

void fillMedia(ProjectData& d, Timeline& tl, const SlotRef& r, const MediaInfo& m, const Value& v, const Context& ctx,
               SlotReport& rep)
{
    const double fps = d.format.rate.fps();
    const IdGen newId = [&d] { return ++d.lastClipId; };
    Clip base = *TimelineOps::findClip(tl, r.clip);
    // A former Speed fit's slow motion is not the slot's own speed: the new file starts from the slot's own one
    const double ownSpeed = base.slot.ownSpeed > 0 ? base.slot.ownSpeed : (base.speed > 0 ? base.speed : 1.0);
    base.speed = ownSpeed;
    const QSizeF frame = frameOf(d, r.sequence);
    const bool video = r.track.kind == TrackKind::Video;

    // What goes with the parts: linked sound (its first clip per audio track is the model for the new one) and kept
    // region copies of the old file
    QSet<int> links;
    for (int id : r.parts)
        if (const Clip* c = TimelineOps::findClip(tl, id); c && c->linkId) links.insert(c->linkId);
    struct Sound {
        int track;
        Clip clip;
    };
    QVector<Sound> sound;
    QVector<int> gone = r.parts;
    if (video)
        for (TrackKind k : {TrackKind::Audio, TrackKind::Video})
            for (int t = 0; t < tl.tracks(k).size(); ++t)
                for (const Clip& a : tl.tracks(k)[t].clips) {
                    if (!a.linkId || !links.contains(a.linkId) || r.parts.contains(a.id)) continue;
                    if (k == TrackKind::Video && a.keepRegion.isEmpty()) continue;
                    gone << a.id;
                    if (k == TrackKind::Audio && a.linkId == base.linkId
                        && std::none_of(sound.begin(), sound.end(), [&](const Sound& s) { return s.track == t; }))
                        sound << Sound{t, a};
                }
    for (int id : gone) TimelineOps::removeClip(tl, id);

    const Range rg = rangeOf(m, v);
    Parts parts;
    bool fitted = false;
    if (r.slot.kind == SlotKind::Music && r.slot.fitMusic && ctx.beats && v.from < 0 && v.to < 0) {
        // Flexible: the whole song within [minLen, maxLen]; Fixed: the slot's own length
        int T = r.slotLength;
        if (r.slot.length == SlotLength::Flexible) {
            T = std::max({1, m.length, r.slot.minLen});
            if (r.slot.maxLen > 0) T = std::min(T, r.slot.maxLen);
        }
        const std::optional<Beats::Result> beats = ctx.beats(m.path, base.audioStream < m.audioStreamCount() ? base.audioStream : 0);
        if (beats && beats->found) {
            MusicFit::Options o;
            o.length = T / fps;
            const MusicFit::Plan pl = MusicFit::plan(*beats, m.length / fps, o);
            if (pl.ok) {
                Clip like = prepared(base, m);
                like.keys.clear(); // they do not fit the cut song (like fit_music)
                like.transIn = like.transOut = 0;
                parts.clips = MusicFit::clips(pl, like, fps, r.start, m.length);
                if (!parts.clips.isEmpty()) {
                    parts.clips.first().id = base.id;
                    parts.length = parts.clips.last().end() - r.start;
                    fitted = true;
                    rep.adjusted << "fit_music";
                    rep.details["cuts"] = int(pl.cuts.size());
                    rep.details["value_s"] = round3(m.length / fps);
                    setText(rep, QString("song cut to %1 on its phrases (%2 cuts)").arg(secs(parts.length, fps)).arg(pl.cuts.size()));
                }
            } else {
                rep.warnings << "fit_music: " + pl.error + "; trimmed instead";
            }
        } else {
            rep.warnings << "no beats found: the song is trimmed instead";
        }
    }
    if (!fitted) parts = plan(base, r, m, rg, fps, rep);
    // A Fixed slot left shorter keeps its own length, a Speed fit the slot's own speed for the next fill
    for (Clip& c : parts.clips) {
        c.slot.nominalLen = r.slot.length == SlotLength::Fixed && parts.length < r.slotLength ? r.slotLength : 0;
        c.slot.ownSpeed = std::abs(c.speed - ownSpeed) > 1e-9 ? ownSpeed : 0;
    }
    if (r.slot.kind == SlotKind::Music) {
        if (r.slot.snapCuts) rep.warnings << "snapCuts: cuts are not moved onto the beat by the fill (op snap_to_beat)";
        for (Clip& c : parts.clips) c.linkId = 0; // a new song is not in sync with a picture
        if (!fitted && rep.adjusted.contains("trimmed")) {
            Clip& last = parts.clips.last();
            last.fadeOut = std::max(last.fadeOut, std::min(int(std::lround(kMusicFadeOut * fps)), last.length()));
            rep.adjusted << "faded";
            setText(rep, "1 s fade-out");
        }
    }

    const int delta = parts.length - r.length;
    if (r.slot.length == SlotLength::Flexible && delta) {
        // Titles and stills on other video tracks that end with the slot (a topic title, a logo over the whole clip, a
        // lower third up to its end) keep ending with it when it grows (when it shrinks, the ripple shortens them at
        // their end anyway); their fades stay as they are
        QVector<int> covering;
        if (delta > 0)
            for (const Track& t : tl.video) {
                if (t.locked) continue;
                for (const Clip& c : t.clips) {
                    if (c.start < r.start || c.end() != r.start + r.length) continue;
                    const bool still = std::any_of(d.media.cbegin(), d.media.cend(), [&c](const MediaInfo& mi) {
                        return mi.path == c.mediaPath && mi.isImage;
                    });
                    if (c.isTitle() || (c.kind == ClipKind::Media && still)) covering << c.id;
                }
            }
        ripple(tl, r.start + r.length, delta, newId);
        for (int id : covering)
            if (Clip* c = TimelineOps::findClip(tl, id)) c->out += delta;
        rep.adjusted << "rippled";
        rep.details["ripple_s"] = round3(delta / fps);
        setText(rep, QString("later clips moved by %1%2").arg(delta > 0 ? "+" : "", secs(delta, fps)));
        int followed = 0;
        followCompounds(d, r.sequence, r.start + r.length, delta, newId, rep, followed, 0);
        if (followed) {
            rep.adjusted << "compound";
            rep.details["compound_clips"] = followed;
            setText(rep, QString("%1 compound clip(s) showing it follow").arg(followed));
        }
    }
    rep.details["length_s"] = round3(parts.length / fps);

    // A placeholder without sound of its own (a still, a silent file): the new file brings its sound, linked to the
    // picture, like a drop of the file (the old one had none to keep). One with sound whose linked audio was taken
    // away stays silent: that was the template's choice.
    if (video && r.slot.kind == SlotKind::Clip && sound.isEmpty() && m.hasAudio && !m.isImage) {
        const std::optional<MediaInfo> old = mediaOf(d, ctx, base.mediaPath);
        if (!old || old->isImage || !old->hasAudio) {
            const int from = r.start, to = r.start + parts.length;
            const auto isFree = [&](int i) {
                if (i < 0 || i >= tl.audio.size() || tl.audio[i].locked) return false;
                const QVector<Clip>& cs = tl.audio[i].clips;
                return std::none_of(cs.begin(), cs.end(), [&](const Clip& c) { return c.start < to && c.end() > from; });
            };
            int a = r.track.index;
            if (!isFree(a)) {
                a = int(tl.audio.size());
                for (int i = 0; i < tl.audio.size() && a == tl.audio.size(); ++i)
                    if (isFree(i)) a = i;
            }
            TimelineOps::ensureTracks(tl, TrackKind::Audio, a + 1);
            Clip model;
            model.mediaPath = m.path;
            sound << Sound{a, model};
            const int link = ++d.lastLinkId;
            for (Clip& c : parts.clips)
                if (!c.freeze) c.linkId = link; // repeats get their own below
            rep.adjusted << "sound_added";
            setText(rep, QString("the file's sound added on A%1").arg(a + 1));
        }
    }

    Track& track = tl.track(r.track);
    for (int i = 0; i < parts.clips.size(); ++i) {
        Clip& c = parts.clips[i];
        if (i > 0) {
            c.id = newId();
            if (c.linkId && !c.freeze) c.linkId = ++d.lastLinkId; // a loop repeat: its own sound link
        }
        if (r.slot.kind == SlotKind::Clip)
            reframePart(c, base, r, m, rg.length < 0 ? 0 : int(std::lround(c.in * c.speed)), frame, ctx, rep);
        else if (r.slot.kind == SlotKind::Logo) {
            SlotReport once; // the report counts it once (loop repeats get the same zoom)
            keepWidth(c, base, m, frame, ctx, i == 0 ? rep : once);
        }
        TimelineOps::placeClip(track, c, newId, tl.frameRate);
        rep.clips << c.id;
    }
    if (video && !sound.isEmpty()) {
        if (!m.hasAudio) {
            rep.adjusted << "no_sound";
            setText(rep, "the file has no sound: linked audio removed");
        } else {
            const auto sounding = [&](int i) { return !parts.clips[i].freeze && parts.clips[i].linkId; };
            int lastSounding = 0; // a frozen tail is silent: the sound's own end (fade) goes on the part before it
            for (int i = 0; i < parts.clips.size(); ++i)
                if (sounding(i)) lastSounding = i;
            for (int i = 0; i < parts.clips.size(); ++i) {
                const Clip& p = parts.clips[i];
                if (!sounding(i)) continue;
                for (const Sound& s : sound) {
                    Clip a = prepared(s.clip, m);
                    a.keys = s.clip.keys;
                    shiftKeys(a, p.in - s.clip.in);
                    a.id = i == 0 && s.clip.id ? s.clip.id : newId(); // the first part keeps the sound clips' ids
                    a.start = p.start;
                    a.in = p.in;
                    a.out = p.out;
                    a.speed = p.speed;
                    a.linkId = p.linkId;
                    if (i > 0) a.transIn = 0, a.fadeIn = 0, a.transInAlone = false;
                    if (i < lastSounding) a.transOut = 0, a.fadeOut = 0, a.transOutAlone = false;
                    TimelineOps::placeClip(tl.audio[s.track], a, newId, tl.frameRate);
                }
            }
        }
    }
    TimelineOps::resolvePendingLinks(tl, [&d] { return ++d.lastLinkId; });
    if (!rep.adjusted.contains("replaced")) rep.adjusted.prepend("replaced");
    if (rep.text.isEmpty()) rep.text = "replaced";
}

} // namespace

QJsonObject Report::toJson() const
{
    const auto problemJson = [](const Problem& p) {
        return QJsonObject{{"slot", p.slot}, {"code", p.code}, {"message", p.message}};
    };
    QJsonArray list, errs;
    for (const SlotReport& s : results) {
        QJsonArray clips;
        for (int id : s.clips) clips << id;
        QJsonObject o{{"slot", s.slot}, {"sequence", s.sequence}, {"clips", clips}, {"ok", s.ok},
                      {"adjusted", QJsonArray::fromStringList(s.adjusted)}, {"details", s.details}, {"text", s.text}};
        if (!s.warnings.isEmpty()) o["warnings"] = QJsonArray::fromStringList(s.warnings);
        if (!s.ok) o["error"] = problemJson(s.error);
        list << o;
    }
    for (const Problem& p : errors) errs << problemJson(p);
    return {{"ok", ok()}, {"results", list}, {"errors", errs}, {"unfilled", QJsonArray::fromStringList(unfilled)}};
}

QVector<SlotRef> listSlots(const ProjectData& d)
{
    QVector<SlotRef> out;
    for (const auto& [seq, tl] : timelines(d))
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
            const QVector<Track>& tracks = tl->tracks(k);
            for (int t = 0; t < tracks.size(); ++t)
                for (const Clip& c : tracks[t].clips) {
                    if (!c.slot.isSet()) continue;
                    if (!out.isEmpty()) { // a further part of the instance before it?
                        SlotRef& last = out.last();
                        if (last.sequence == seq && last.track == TrackRef{k, t} && last.slot.id == c.slot.id
                            && last.start + last.length == c.start) {
                            last.parts << c.id;
                            last.length += c.length();
                            continue;
                        }
                    }
                    SlotRef r;
                    r.slot = c.slot;
                    r.sequence = seq;
                    r.track = {k, t};
                    r.clip = c.id;
                    r.parts = {c.id};
                    r.start = c.start;
                    r.length = c.length();
                    if (c.isTitle()) r.text = c.title.text;
                    else r.media = c.mediaPath;
                    out << r;
                }
        }
    // A Fixed slot a shorter value left shorter: its own length, as far as its track is free after it - and the
    // tracks of its linked sound, which the fill lays along with the picture
    for (SlotRef& r : out) {
        r.slotLength = r.length;
        if (r.slot.length != SlotLength::Fixed || r.slot.nominalLen <= r.length) continue;
        const int end = r.start + r.length;
        int limit = r.start + r.slot.nominalLen;
        for (const auto& [seq, tl] : timelines(d)) {
            if (seq != r.sequence) continue;
            QSet<int> links;
            for (int id : r.parts)
                if (const Clip* c = TimelineOps::findClip(*tl, id); c && c->linkId) links.insert(c->linkId);
            const auto linked = [&](const Clip& c) { return c.linkId && links.contains(c.linkId); };
            const auto freeUntil = [&](const Track& t) {
                for (const Clip& c : t.clips)
                    if (c.start >= end && !linked(c)) limit = std::min(limit, c.start);
            };
            freeUntil(tl->track(r.track));
            if (r.track.kind == TrackKind::Video && !links.isEmpty())
                for (const Track& t : tl->audio)
                    if (std::any_of(t.clips.begin(), t.clips.end(), linked)) freeUntil(t);
        }
        r.slotLength = std::max(r.length, limit - r.start);
    }
    return out;
}

QStringList slotIds(const ProjectData& d)
{
    QStringList ids;
    for (const SlotRef& r : listSlots(d))
        if (!ids.contains(r.slot.id)) ids << r.slot.id;
    return ids;
}

QString canHold(const Clip& c, TrackKind track, SlotKind kind)
{
    switch (kind) {
    case SlotKind::Clip:
    case SlotKind::Logo:
        if (track != TrackKind::Video || c.kind != ClipKind::Media || !c.keepRegion.isEmpty())
            return kind == SlotKind::Clip ? "a clip slot must be a video or still clip of a file on a video track"
                                          : "a logo slot must be a still or video clip of a file on a video track";
        return {};
    case SlotKind::Text:
        if (track != TrackKind::Video || !c.isTitle() || c.title.kind != TitleKind::Text)
            return "a text slot must be a text title";
        return {};
    case SlotKind::Music:
        if (track != TrackKind::Audio || c.kind != ClipKind::Media) return "a music slot must be an audio clip of a file";
        return {};
    }
    return {};
}

QString canFill(SlotKind kind, const MediaInfo& m)
{
    switch (kind) {
    case SlotKind::Text: return "a text slot takes a text, not a file";
    case SlotKind::Music: return m.hasAudio ? QString() : "the file has no sound (a music slot)";
    case SlotKind::Clip:
    case SlotKind::Logo: return m.hasVideo || m.isImage ? QString() : "the file has no picture";
    }
    return {};
}

std::optional<Problem> checkSlot(const ProjectData& d, const SlotRef& r, const Value& v, const Context& ctx)
{
    return checkOne(d, r, v, ctx);
}

QVector<Problem> check(const ProjectData& d, const Values& values, const Context& ctx)
{
    QVector<Problem> out;
    const QVector<SlotRef> all = listSlots(d);
    for (auto it = values.begin(); it != values.end(); ++it) {
        bool found = false;
        for (const SlotRef& r : all) {
            if (r.slot.id != it.key()) continue;
            found = true;
            if (const std::optional<Problem> p = checkOne(d, r, it.value(), ctx); p && !out.contains(*p)) out << *p;
        }
        if (!found) out << problem(it.key(), "UNKNOWN_SLOT", QString("the project has no slot '%1'").arg(it.key()));
    }
    return out;
}

Report apply(ProjectData& d, const Values& values, const Context& ctx)
{
    Report rep;
    for (const QString& id : slotIds(d))
        if (!values.contains(id)) rep.unfilled << id;
    for (auto it = values.begin(); it != values.end(); ++it) {
        // The instances by (timeline, first clip): a fill keeps the first clip's id, positions may move
        QVector<QPair<int, int>> keys;
        for (const SlotRef& r : listSlots(d))
            if (r.slot.id == it.key()) keys.append({r.sequence, r.clip});
        if (keys.isEmpty()) {
            rep.errors << problem(it.key(), "UNKNOWN_SLOT", QString("the project has no slot '%1'").arg(it.key()));
            continue;
        }
        for (const auto& [seq, clip] : keys) {
            std::optional<SlotRef> r;
            for (const SlotRef& x : listSlots(d))
                if (x.sequence == seq && x.clip == clip) r = x;
            if (!r) continue;
            SlotReport sr;
            sr.slot = r->slot.id;
            sr.sequence = seq;
            if (const std::optional<Problem> p = checkOne(d, *r, it.value(), ctx)) {
                sr.ok = false;
                sr.error = *p;
                sr.clips = r->parts;
                sr.text = p->message;
                if (!rep.errors.contains(*p)) rep.errors << *p;
                rep.results << sr;
                continue;
            }
            Timeline& tl = *timelineOf(d, seq);
            if (r->slot.kind == SlotKind::Text) {
                fillText(tl, *r, it.value(), sr);
                sr.clips = r->parts;
            } else {
                const MediaInfo m = *mediaOf(d, ctx, it.value().file);
                if (std::none_of(d.media.begin(), d.media.end(), [&](const MediaInfo& x) { return x.path == m.path; }))
                    d.media << m;
                fillMedia(d, tl, *r, m, it.value(), ctx, sr);
            }
            rep.results << sr;
        }
    }
    // A project from Project::data() carries the open timeline in its sequence too
    for (Sequence& s : d.sequences)
        if (s.id == d.currentSequence) s.timeline = d.timeline;
    return rep;
}

} // namespace TemplateFill
