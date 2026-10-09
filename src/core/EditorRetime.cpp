// Editor: clip speed and speed ramps.
#include "core/Editor.h"
#include "core/EditorDetail.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Presets.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Retime.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <algorithm>

using EditorDetail::fitTransitions;

void Editor::setClipSpeed(const QVector<int>& ids, const Retime& r, bool ripple)
{
    const QVector<int> all = withLinked(ids);
    if (all.isEmpty() || r.speed <= 0) return;
    m_project->edit(T("Geschwindigkeit ändern"), [&](Timeline& tl) {
        QSet<int> touched;
        const QVector<TrackRef> edited = TimelineOps::tracksOf(tl, all);
        QVector<QPair<int, int>> shifts; // Ripple für die übrigen Spuren (ab altem Ende, Längenänderung)
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (Track& t : tl.tracks(k)) {
                // von hinten, damit Ripple-Verschiebungen sich nicht gegenseitig verfälschen
                for (int i = t.clips.size() - 1; i >= 0; --i) {
                    Clip& c = t.clips[i];
                    if (!all.contains(c.id) || c.isTitle()) continue;
                    const MediaInfo* m = m_project->mediaInfo(c.mediaPath);
                    if (!m || m->isImage || m->length <= 0) continue;
                    const int L = m->length;
                    // Konstantes Tempo ersetzt eine Speed Ramp (wie DaVinci „Change Clip Speed“)
                    Clip n = c;
                    n.speed = r.speed;
                    n.reverse = r.reverse;
                    n.freeze = r.freeze;
                    n.ramp.clear();
                    const RetimeMap oldMap(c, L), newMap(n, L);
                    // Ausschnitt in Datei-Frames (a = Anfang des Clips, b = Ende, bei rückwärts a > b)
                    const double a = oldMap.fileFrameAt(c.in);
                    const double b = oldMap.fileFrameAt(c.out);
                    const bool flip = r.reverse != c.reverse;
                    // Neuer Anfang: gleiches Datei-Frame; bei Richtungswechsel dasselbe Stück umgekehrt
                    const double startFile = flip ? b : a;
                    const int avail = std::max(1, int(L / r.speed));
                    const int newIn = std::clamp(int(std::lround(newMap.materialAtFile(startFile))), 0, avail - 1);
                    const int oldLen = c.length();
                    int newLen = oldLen;
                    if (ripple && !r.freeze && !c.freeze) {
                        // Quell-Spanne des Clips (bei Speed Ramp nicht einfach Länge · Tempo)
                        const double span = c.hasRamp() ? oldMap.sourceAt(c.out + 1) - oldMap.sourceAt(c.in)
                                                        : oldLen * c.speed;
                        newLen = std::max(1, int(std::lround(span / r.speed)));
                    }
                    if (!r.freeze) newLen = std::min(newLen, avail - newIn);
                    // Keyframes über die Datei-Frames umrechnen
                    for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
                        KeyTrack mapped = it.value(), moved;
                        for (Keyframe& kf : mapped)
                            kf.frame = int(std::lround(newMap.materialAtFile(oldMap.fileFrameAt(kf.frame))));
                        Keys::rescaleHandles(it.value(), mapped);
                        for (const Keyframe& kf : mapped)
                            if (std::none_of(moved.begin(), moved.end(), [&](const Keyframe& o) { return o.frame == kf.frame; }))
                                moved << kf;
                        std::sort(moved.begin(), moved.end(),
                                  [](const Keyframe& x, const Keyframe& y) { return x.frame < y.frame; });
                        it.value() = moved;
                    }
                    const int oldEnd = c.end();
                    c.in = newIn;
                    c.out = newIn + newLen - 1;
                    c.speed = r.speed;
                    c.reverse = r.reverse;
                    c.freeze = r.freeze;
                    c.freezeFrame = r.freeze ? newIn : -1; // Bild am Clipanfang (bleibt beim Teilen/Trimmen)
                    c.keepPitch = r.keepPitch;
                    c.ramp.clear();
                    c.fadeIn = std::min(c.fadeIn, newLen);
                    c.fadeOut = std::min(c.fadeOut, newLen);
                    touched.insert(c.id);
                    if (const int delta = newLen - oldLen; ripple && delta != 0) {
                        for (int j = i + 1; j < t.clips.size(); ++j)
                            if (t.clips[j].start >= oldEnd) t.clips[j].start += delta;
                        // verknüpfte Partner liefern denselben Versatz -> nur einmal
                        if (!shifts.contains(qMakePair(oldEnd, delta))) shifts << qMakePair(oldEnd, delta);
                    }
                }
            }
        TimelineOps::rippleTracks(tl, shifts, edited);
        fitTransitions(tl, touched, sourceLength());
    });
}

// ---- Speed Ramp (Retime Controls) ----

bool Editor::canRetime(int clipId) const
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || c->isTitle() || c->isCompound() || c->freeze) return false;
    const MediaInfo* m = m_project->mediaInfo(c->mediaPath);
    return m && !m->isImage && m->length > 0;
}

void Editor::editRamp(const QString& text, int clipId, const std::function<bool(Clip&, const RetimeMap&, int)>& change)
{
    if (!canRetime(clipId) || !editable({clipId}).contains(clipId)) return;
    const Timeline& cur = m_project->timeline();
    const Clip* first = TimelineOps::findClip(cur, clipId);
    const MediaInfo* m = m_project->mediaInfo(first->mediaPath);
    const int L = m->length;
    // Partner derselben Datei machen mit (Video + Ton), fremde Partner (anderer Ton) nicht
    QVector<int> ids;
    for (int id : withLinked({clipId}))
        if (const Clip* c = TimelineOps::findClip(cur, id); c && c->mediaPath == first->mediaPath && !c->freeze)
            ids << id;
    // Erst am Hauptclip prüfen, ob sich etwas ändert (kein leerer Undo-Schritt)
    {
        Clip probe = *first;
        if (!change(probe, RetimeMap(*first, L), L)) return;
    }
    Timeline tl = cur;
    QSet<int> touched;
    QVector<QPair<int, int>> shifts;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : tl.tracks(k))
            for (int i = t.clips.size() - 1; i >= 0; --i) {
                Clip& c = t.clips[i];
                if (!ids.contains(c.id)) continue;
                const RetimeMap oldMap(c, L);
                // Quellstellen merken: Anfang, Ende (exklusiv), Keyframes
                const double sIn = oldMap.sourceAt(c.in), sEnd = oldMap.sourceAt(c.out + 1);
                QMap<AnimParam, QVector<double>> keySrc;
                for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it)
                    for (const Keyframe& kf : it.value()) keySrc[it.key()] << oldMap.sourceAt(kf.frame);
                Clip n = c;
                if (!change(n, oldMap, L)) continue;
                ::Retime::normalize(n.ramp, L);
                const RetimeMap newMap(n, L);
                const int avail = std::max(1, newMap.length());
                n.in = std::clamp(int(std::lround(newMap.materialAt(sIn))), 0, avail - 1);
                const int len = std::clamp(int(std::lround(newMap.materialAt(sEnd))) - n.in, 1, avail - n.in);
                n.out = n.in + len - 1;
                for (auto it = n.keys.begin(); it != n.keys.end(); ++it) {
                    KeyTrack mapped = it.value(), moved;
                    const QVector<double>& src = keySrc[it.key()];
                    mapped.resize(std::min(mapped.size(), src.size()));
                    for (int j = 0; j < mapped.size(); ++j) mapped[j].frame = int(std::lround(newMap.materialAt(src[j])));
                    Keys::rescaleHandles(it.value(), mapped);
                    for (const Keyframe& kf : mapped)
                        if (moved.isEmpty() || moved.last().frame != kf.frame) moved << kf;
                    it.value() = moved;
                }
                n.fadeIn = std::min(n.fadeIn, len);
                n.fadeOut = std::min(n.fadeOut, len);
                const int oldEnd = c.end();
                const int delta = n.length() - c.length();
                c = n;
                touched.insert(c.id);
                if (delta != 0) {
                    for (int j = i + 1; j < t.clips.size(); ++j)
                        if (t.clips[j].start >= oldEnd) t.clips[j].start += delta;
                    if (!shifts.contains(qMakePair(oldEnd, delta))) shifts << qMakePair(oldEnd, delta);
                }
            }
    if (touched.isEmpty()) return;
    TimelineOps::rippleTracks(tl, shifts, TimelineOps::tracksOf(tl, ids));
    fitTransitions(tl, touched, sourceLength());
    m_project->edit(text, [&](Timeline& t) { t = tl; });
}

void Editor::addSpeedPoint(int clipId, int frame)
{
    const Clip* c = TimelineOps::findClip(m_project->timeline(), clipId);
    if (!c || frame <= c->start || frame >= c->end()) return;
    const int m = c->in + frame - c->start;
    editRamp(T("Speed-Punkt hinzufügen"), clipId, [m](Clip& x, const RetimeMap& map, int) {
        const double s = map.sourceAt(m);
        for (const SpeedPoint& p : x.ramp)
            if (std::abs(p.source - s) < 1) return false;
        SpeedPoint p;
        p.source = s;
        // neuer Abschnitt behält das Tempo der Stelle (harter Wert des Abschnitts, ohne Übergang)
        int seg = 0;
        while (seg < x.ramp.size() && x.ramp[seg].source <= s) ++seg;
        p.speed = ::Retime::segmentSpeed(x, seg);
        x.ramp << p;
        std::sort(x.ramp.begin(), x.ramp.end(), [](const SpeedPoint& a, const SpeedPoint& b) { return a.source < b.source; });
        return true;
    });
}

void Editor::removeSpeedPoint(int clipId, int index)
{
    editRamp(T("Speed-Punkt entfernen"), clipId, [index](Clip& x, const RetimeMap&, int) {
        if (index < 0 || index >= x.ramp.size()) return false;
        x.ramp.removeAt(index);
        return true;
    });
}

void Editor::moveSpeedPoint(int clipId, int index, int delta)
{
    if (delta == 0) return;
    editRamp(T("Speed-Punkt verschieben"), clipId, [index, delta](Clip& x, const RetimeMap&, int L) {
        if (index < 0 || index >= x.ramp.size()) return false;
        // Tempo davor läuft weiter bzw. endet früher -> neue Quellstelle aus dem Tempo des Abschnitts davor
        const double before = ::Retime::segmentSpeed(x, index);
        const double lo = (index > 0 ? x.ramp[index - 1].source : 0) + 1;
        const double hi = (index + 1 < x.ramp.size() ? x.ramp[index + 1].source : L) - 1;
        const double s = std::clamp(x.ramp[index].source + delta * before, lo, hi);
        if (std::abs(s - x.ramp[index].source) < 1e-6) return false;
        x.ramp[index].source = s;
        return true;
    });
}

void Editor::setSegmentSpeed(int clipId, int segment, double speed)
{
    if (speed <= 0) return;
    editRamp(T("Geschwindigkeit ändern"), clipId, [segment, speed](Clip& x, const RetimeMap&, int) {
        if (segment < 0 || segment > x.ramp.size()) return false;
        const double before = ::Retime::segmentSpeed(x, segment);
        ::Retime::setSegmentSpeed(x, segment, speed);
        return ::Retime::segmentSpeed(x, segment) != before;
    });
}

void Editor::setSpeedPointSmooth(int clipId, int index, int frames)
{
    editRamp(T("Übergang des Speed-Punkts"), clipId, [index, frames](Clip& x, const RetimeMap&, int) {
        if (index < 0 || index >= x.ramp.size() || x.ramp[index].smooth == std::max(0, frames)) return false;
        x.ramp[index].smooth = std::max(0, frames);
        return true;
    });
}
