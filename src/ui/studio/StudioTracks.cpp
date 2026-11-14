// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/studio/StudioTracks.h"

#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Automation.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "core/Voiceover.h"
#include "engine/MediaCache.h"
#include "ui/EffectsLibrary.h"

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>

#include <algorithm>
#include <climits>
#include <array>
#include <cmath>
#include <optional>

namespace {

QColor trackTint(const Track& t)
{
    const TrackColorInfo* info = trackColorInfo(t.color);
    return info ? QColor::fromRgba(info->rgb) : Theme::audioClip;
}

} // namespace

StudioTracks::StudioTracks(Editor* editor, MediaCache* cache, QWidget* parent)
    : QWidget(parent), m_editor(editor), m_cache(cache)
{
    setObjectName("studio_tracks");
    setProperty("tourTarget", "studio_tracks");
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setAcceptDrops(true); // effect sounds
    setMinimumSize(320, kRulerH + kMinLaneH + 16);
    m_hbar = new QScrollBar(Qt::Horizontal, this);
    m_vbar = new QScrollBar(Qt::Vertical, this);
    m_vbar->hide();
    // The scrollbars are user input: they move the view (and only they, the wheel and Fit)
    connect(m_hbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (m_syncing) return;
        m_view.leftFrame = std::max(0.0, v / m_view.pxPerFrame);
        syncScrollBars();
        update();
    });
    connect(m_vbar, &QScrollBar::valueChanged, this, [this](int v) {
        if (m_syncing) return;
        m_scrollY = v;
        update();
    });
    Project* project = m_editor->project();
    connect(project, &Project::timelineChanged, this, [this] {
        layoutLanes(); // a track more or less: lane heights and ranges, never zoom or scroll
        update();
    });
    if (m_cache) connect(m_cache, &MediaCache::updated, this, qOverload<>(&QWidget::update));
    connect(m_editor->selection(), &Selection::changed, this, qOverload<>(&QWidget::update));
}

void StudioTracks::setPlayhead(int frame)
{
    if (frame == m_playhead) return;
    // The view stays where it is (no follow while playing: core rule). Only the old and the new playhead column and
    // the timecode corner are drawn anew (playing moves it every frame; the lanes under it stay as they are)
    const int old = m_playhead;
    m_playhead = frame;
    update(playheadRect(old));
    update(playheadRect(frame));
    update(QRect(0, 0, kHeaderW, kRulerH));
}

QRect StudioTracks::playheadRect(int frame) const
{
    // The line (1 px at x + 0.5) and the ruler's triangle (x - 5 .. x + 6), with a pixel to spare for rounding
    const int x = int(std::floor(xOf(frame)));
    return QRect(x - 7, 0, 15, height());
}

void StudioTracks::setPenTarget(PenTarget target)
{
    if (target == m_penTarget) return;
    m_penTarget = target;
    update(); // the track level line shows in track mode
}

void StudioTracks::setTool(Tool tool)
{
    if (tool == m_tool) return;
    m_tool = tool;
    m_drag = Drag::None;
    m_stroke.clear();
    setCursor(tool == Tool::Pen ? Qt::CrossCursor : Qt::ArrowCursor);
    emit toolChanged(tool);
    update();
}

void StudioTracks::setView(double pxPerFrame, double leftFrame)
{
    m_view.pxPerFrame = std::clamp(pxPerFrame, ViewState::kMinPxPerFrame, ViewState::kMaxPxPerFrame);
    m_view.leftFrame = std::max(0.0, leftFrame);
    syncScrollBars();
    update();
}

void StudioTracks::fit()
{
    const int end = std::max(1, TimelineOps::endFrame(m_editor->project()->timeline()));
    const int w = std::max(1, clipArea().width() - 12);
    setView(double(w) / end, 0);
}

QRect StudioTracks::clipArea() const
{
    const int sb = m_hbar->sizeHint().height();
    const int vb = m_vbar->isVisible() ? m_vbar->sizeHint().width() : 0;
    return QRect(kHeaderW, kRulerH, std::max(0, width() - kHeaderW - vb), std::max(0, height() - kRulerH - sb));
}

double StudioTracks::xOf(double frame) const { return kHeaderW + (frame - m_view.leftFrame) * m_view.pxPerFrame; }

double StudioTracks::frameAt(double x) const { return m_view.leftFrame + (x - kHeaderW) / m_view.pxPerFrame; }

QRect StudioTracks::laneRect(int track) const
{
    const QRect area = clipArea();
    return QRect(area.left(), area.top() + track * m_laneH - m_scrollY, area.width(), m_laneH);
}

QRect StudioTracks::headerRect(int track) const
{
    const QRect lane = laneRect(track);
    return QRect(0, lane.top(), kHeaderW, lane.height());
}

QRect StudioTracks::muteRect(int track) const
{
    const QRect h = headerRect(track);
    return QRect(h.left() + 10, h.bottom() - 24, 24, 18);
}

QRect StudioTracks::soloRect(int track) const { return muteRect(track).translated(28, 0); }

int StudioTracks::trackAt(int y) const
{
    const QRect area = clipArea();
    if (y < area.top() || y > area.bottom()) return -1;
    const int t = (y - area.top() + m_scrollY) / std::max(1, m_laneH);
    return t < m_editor->project()->timeline().audio.size() ? t : -1;
}

const Clip* StudioTracks::clipAt(int track, double frame) const
{
    const Timeline& tl = m_editor->project()->timeline();
    if (track < 0 || track >= tl.audio.size()) return nullptr;
    for (const Clip& c : tl.audio[track].clips)
        if (frame >= c.start && frame < c.end()) return &c;
    return nullptr;
}

double StudioTracks::dbAtY(int track, int y) const
{
    const QRect lane = laneRect(track).adjusted(0, 4, 0, -4);
    return VolumeDraw::dbAt(double(lane.bottom() - y) / std::max(1, lane.height() - 1));
}

void StudioTracks::layoutLanes()
{
    const int tracks = std::max(1, laneCount());
    const int sb = m_hbar->sizeHint().height();
    const int room = std::max(0, height() - kRulerH - sb);
    // Tall lanes that fill the room (a few tracks: big waveforms); many tracks scroll at the minimum height
    m_laneH = std::clamp(room / int(tracks), kMinLaneH, 220);
    const int content = m_laneH * tracks;
    const bool scroll = content > room;
    m_vbar->setVisible(scroll);
    if (!scroll) m_scrollY = 0; // all lanes in view: nothing to scroll (not a move of the view)
    m_scrollY = std::clamp(m_scrollY, 0, std::max(0, content - room));
    syncScrollBars();
}

void StudioTracks::syncScrollBars()
{
    m_syncing = true;
    const QRect area = clipArea();
    const int sb = m_hbar->sizeHint().height();
    const int vb = m_vbar->sizeHint().width();
    m_hbar->setGeometry(kHeaderW, height() - sb, std::max(0, width() - kHeaderW - (m_vbar->isVisible() ? vb : 0)), sb);
    m_vbar->setGeometry(width() - vb, kRulerH, vb, area.height());
    // Range: the project's end or the view's, whichever is further, and a screen more - never smaller than where it is
    const double visible = area.width() / m_view.pxPerFrame;
    const double end = std::max<double>(TimelineOps::endFrame(m_editor->project()->timeline()), m_view.leftFrame + visible);
    const double px = m_view.pxPerFrame;
    m_hbar->setRange(0, int(std::min(2e9, (end + visible) * px - area.width())));
    m_hbar->setPageStep(std::max(1, area.width()));
    m_hbar->setSingleStep(std::max(1, area.width() / 20));
    m_hbar->setValue(int(std::lround(m_view.leftFrame * px)));
    const int content = m_laneH * laneCount();
    m_vbar->setRange(0, std::max(0, content - area.height()));
    m_vbar->setPageStep(std::max(1, area.height()));
    m_vbar->setSingleStep(std::max(1, m_laneH / 4));
    m_vbar->setValue(m_scrollY);
    m_syncing = false;
}

void StudioTracks::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutLanes();
}

// ---------- Painting ----------

void StudioTracks::paintEvent(QPaintEvent* e)
{
    ++m_paints;
    // The waveforms and curves are drawn only where needed: part by part of the region (a playhead move: its two
    // columns and the timecode corner, not everything between them)
    QPainter p(this);
    for (const QRect& r : e->region()) {
        m_dirty = r;
        p.save();
        p.setClipRect(r);
        paintPart(p);
        p.restore();
    }
    m_painted = e->region();
}

void StudioTracks::paintPart(QPainter& p)
{
    p.fillRect(m_dirty, Theme::timelineBg);
    const Timeline& tl = m_editor->project()->timeline();
    const QRect area = clipArea();
    p.save();
    p.setClipRect(QRect(0, area.top(), width(), area.height()), Qt::IntersectClip);
    for (int i = 0; i < tl.audio.size(); ++i) drawLane(p, i);
    // A take onto a new track (Automatisch without a free track): a lane for it below the others until it is placed
    if (takeOnNewTrack()) drawNewTrackLane(p, int(tl.audio.size()));
    // The take being recorded grows on its lane (red, its peaks), like in the timeline
    if (m_take && m_takeTrack >= 0 && m_takeTrack <= tl.audio.size() && m_take->lengthFrames() > 0) {
        const QRect body = laneRect(m_takeTrack).adjusted(0, 4, 0, -4);
        const double x0 = std::max<double>(kHeaderW, xOf(m_take->start())), x1 = std::min<double>(area.right(), xOf(m_take->end()));
        if (x1 > x0) {
            const QRect r(int(x0), body.top(), std::max(1, int(x1 - x0)), body.height());
            p.fillRect(r, Theme::alpha(Theme::recording, 90));
            p.setPen(Theme::alpha(Theme::recording.lighter(150), 230));
            const int mid = body.center().y();
            const double half = body.height() / 2.0 - 2;
            for (int x = std::max(r.left(), m_dirty.left()); x <= std::min(r.right(), m_dirty.right()); ++x) {
                const float pk = m_take->peak(frameAt(x), frameAt(x + 1));
                if (pk <= 0) continue;
                const double v = std::max(0.0, 1.0 + 20.0 * std::log10(pk) / 48.0);
                const int h = std::max(1, int(v * half));
                p.drawLine(x, mid - h, x, mid + h);
            }
            p.setPen(QPen(Theme::recording, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(0, 0, -1, -1));
        }
    }
    p.restore();
    if (tl.audio.isEmpty()) {
        p.setPen(Theme::textDim);
        p.drawText(area.adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                   T("Keine Tonspuren: leg Clips mit Ton auf die Timeline"));
    }
    drawRuler(p);
    // The playhead over ruler and lanes
    const double x = xOf(m_playhead);
    if (x >= kHeaderW && x <= area.right()) {
        p.setPen(QPen(Theme::playhead, 1));
        p.drawLine(QPointF(x + 0.5, 2), QPointF(x + 0.5, area.bottom()));
        const QPointF tri[] = {{x - 5, 2}, {x + 6, 2}, {x + 0.5, 9}};
        p.setBrush(Theme::playhead);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri, 3);
    }
    // The stroke being drawn
    if (m_drag == Drag::Pen && m_stroke.size() > 1) {
        const QRect lane = laneRect(m_penTrack).adjusted(0, 4, 0, -4);
        QPainterPath path;
        for (int i = 0; i < m_stroke.size(); ++i) {
            const QPointF pt(xOf(m_stroke[i].frame), lane.bottom() - VolumeDraw::posOf(m_stroke[i].db) * (lane.height() - 1));
            if (i == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Theme::secondary, 2));
        p.setBrush(Qt::NoBrush);
        p.setClipRect(area, Qt::IntersectClip);
        p.drawPath(path);
    }
}

void StudioTracks::drawRuler(QPainter& p)
{
    const int fps = std::max(1, m_editor->project()->fps());
    p.fillRect(QRect(0, 0, width(), kRulerH), Theme::ruler);
    p.setPen(Theme::border);
    p.drawLine(0, kRulerH - 1, width(), kRulerH - 1);
    // Playhead timecode in the corner over the headers
    QFont f = font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.fillRect(QRect(4, 3, kHeaderW - 8, kRulerH - 6), Theme::well);
    p.setPen(Theme::primary);
    p.drawText(QRect(8, 3, kHeaderW - 12, kRulerH - 6), Qt::AlignVCenter | Qt::AlignLeft, Timecode::format(m_playhead, fps));
    const int candidates[] = {1, 2, 5, 10, fps, 2 * fps, 5 * fps, 10 * fps, 15 * fps, 30 * fps, 60 * fps,
                              120 * fps, 300 * fps, 600 * fps, 900 * fps, 1800 * fps, 3600 * fps, 7200 * fps};
    int major = candidates[std::size(candidates) - 1];
    for (int c : candidates)
        if (c * m_view.pxPerFrame >= 110) {
            major = c;
            break;
        }
    int minor = major % 5 == 0 ? major / 5 : (major % 2 == 0 ? major / 2 : major);
    if (minor * m_view.pxPerFrame < 6) minor = major;
    p.save();
    p.setClipRect(QRect(kHeaderW, 0, width() - kHeaderW, kRulerH), Qt::IntersectClip);
    f.setPointSizeF(7.5);
    p.setFont(f);
    const int first = int(m_view.leftFrame) / minor * minor;
    const int last = int(frameAt(width())) + minor;
    for (int fr = first; fr <= last; fr += minor) {
        const int x = int(xOf(fr));
        const bool isMajor = fr % major == 0;
        p.setPen(isMajor ? Theme::textDim : Theme::textFaint);
        p.drawLine(x, kRulerH - (isMajor ? 10 : 5), x, kRulerH - 1);
        if (isMajor) p.drawText(x + 3, 12, Timecode::format(fr, fps));
    }
    p.restore();
}

void StudioTracks::drawLane(QPainter& p, int track)
{
    const Timeline& tl = m_editor->project()->timeline();
    const Track& t = tl.audio[track];
    const QRect lane = laneRect(track);
    if (lane.bottom() < kRulerH || lane.top() > height()) return;
    const bool anySolo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& x) { return x.solo; });
    const bool silent = t.muted || (anySolo && !t.solo);
    p.fillRect(lane, track % 2 ? Theme::trackBgAlt : Theme::trackBg);
    // Volume scale lines: 0 dB and -20 dB (the scale's bends)
    const QRect body = lane.adjusted(0, 4, 0, -4);
    for (double db : {0.0, -20.0}) {
        const int y = body.bottom() - int(std::lround(VolumeDraw::posOf(db) * (body.height() - 1)));
        p.fillRect(QRect(lane.left(), y, lane.width(), 1), Theme::alpha(Theme::controlLight, db == 0 ? 120 : 60));
    }
    const QColor tint = trackTint(t);
    p.save();
    p.setClipRect(clipArea().intersected(lane), Qt::IntersectClip);
    for (const Clip& c : t.clips) {
        const double x0 = xOf(c.start), x1 = xOf(c.end());
        if (x1 < lane.left() || x0 > lane.right()) continue;
        if (x1 < m_dirty.left() - 2 || x0 > m_dirty.right() + 2) continue; // not in the part being drawn
        const QRect r(int(x0), body.top(), std::max(1, int(x1 - x0)), body.height());
        const bool selected = m_editor->selection()->contains(c.id);
        p.fillRect(r, Theme::mix(Theme::trackBg, tint, silent || !c.enabled ? 0.18 : 0.38));
        drawWaveform(p, r, c, silent || !c.enabled ? Theme::controlOff : Theme::mix(tint, Qt::white, 0.6));
        // Name in the top left corner
        QFont f = font();
        f.setPointSizeF(7.5);
        p.setFont(f);
        p.setPen(Theme::alpha(Theme::text, 200));
        p.drawText(r.adjusted(4, 1, -4, 0), Qt::AlignTop | Qt::AlignLeft,
                   QFontMetrics(f).elidedText(c.displayName(), Qt::ElideRight, std::max(0, r.width() - 8)));
        drawVolumeLine(p, r, c);
        p.setPen(QPen(selected ? Theme::clipSelected : Theme::border, selected ? 2 : 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(0, 0, -1, -1));
    }
    drawAutomation(p, track);
    p.restore();
    // Header: color tag, short and display name, M/S
    const QRect h = headerRect(track);
    p.fillRect(h.adjusted(0, 0, 0, -1), Theme::trackHeader);
    p.fillRect(QRect(h.left(), h.top(), 3, h.height() - 1), tint);
    p.fillRect(QRect(h.left(), h.bottom(), width(), 1), Theme::border);
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(9);
    p.setFont(f);
    p.setPen(silent ? Theme::textFaint : Theme::text);
    const QString shortName = trackShortName({TrackKind::Audio, track});
    p.drawText(QRect(h.left() + 10, h.top() + 6, h.width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, shortName);
    f.setBold(false);
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(Theme::textDim);
    if (h.height() >= 70)
        p.drawText(QRect(h.left() + 10, h.top() + 24, h.width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(f).elidedText(trackDisplayName(t, {TrackKind::Audio, track}), Qt::ElideRight, h.width() - 16));
    auto button = [&](const QRect& r, const QString& text, bool on, const QColor& onColor) {
        p.fillRect(r, on ? onColor : Theme::control);
        p.setPen(on ? Theme::readableOn(onColor) : Theme::text);
        QFont bf = font();
        bf.setBold(true);
        bf.setPointSizeF(7.5);
        p.setFont(bf);
        p.drawText(r, Qt::AlignCenter, text);
    };
    button(muteRect(track), QStringLiteral("M"), t.muted, Theme::warning);
    button(soloRect(track), QStringLiteral("S"), t.solo, Theme::meterMid);
    if (t.locked) {
        p.setPen(Theme::textDim);
        p.drawText(soloRect(track).translated(30, 0), Qt::AlignCenter, QStringLiteral("🔒"));
    }
}

void StudioTracks::drawWaveform(QPainter& p, const QRect& body, const Clip& c, const QColor& color)
{
    if (!m_cache || c.isTitle() || c.freeze) return;
    const auto wave = m_cache->waveform(c.mediaPath, c.audioStream, c.sourceChannels);
    if (!wave || wave->levels.isEmpty()) return;
    constexpr int N = Waveform::kBucketsPerFrame;
    const double bucketsPerPx = N / m_view.pxPerFrame;
    int level = 0;
    while (level + 1 < wave->levels.size() && (1 << (level + 1)) <= bucketsPerPx) ++level;
    const QVector<quint8>& lv = wave->levels[level];
    const double scale = double(N) / (1 << level);
    const int available = int(std::min<double>(lv.size(), std::ceil(wave->frames * scale)));
    const MediaInfo* info = m_editor->project()->mediaInfo(c.mediaPath);
    const int fileLen = info && info->length > 0 ? info->length : int(wave->frames);
    const std::optional<RetimeMap> map = c.isRetimed() ? std::optional<RetimeMap>(RetimeMap(c, fileLen)) : std::nullopt;
    const int clipEnd = int(std::min<double>(available, (c.out + 1) * scale));
    // The clip's track (its automation)
    const AutoCurve* autoCurve = nullptr;
    {
        TrackRef ref;
        if (TimelineOps::findClip(m_editor->project()->timeline(), c.id, &ref) && ref.kind == TrackKind::Audio)
            autoCurve = &m_editor->project()->timeline().audio[ref.index].volumeAuto;
    }
    // dB height (-48..0 dB), so speech does not look flat; the waveform follows the volume curve (what is heard)
    static const auto dbTable = [] {
        std::array<double, 256> t{};
        for (int i = 1; i < 256; ++i) t[i] = std::max(0.0, 1.0 + 20.0 * std::log10(i / 255.0) / 48.0);
        return t;
    }();
    const int mid = body.top() + body.height() / 2;
    const double half = body.height() / 2.0 - 2;
    const double clipX = xOf(c.start);
    const int x0 = std::max({body.left(), kHeaderW, m_dirty.left() - 1}), x1 = std::min({body.right(), width() - 1, m_dirty.right() + 1});
    QVector<QLine> lines;
    lines.reserve(std::max(0, x1 - x0 + 1));
    for (int x = x0; x <= x1; ++x) {
        const double t = (x + 0.5 - clipX) / m_view.pxPerFrame;
        double f0 = c.in + (x - clipX) / m_view.pxPerFrame;
        double f1 = f0 + 1 / m_view.pxPerFrame;
        int end = clipEnd;
        if (map) {
            if (f0 >= c.out + 1) continue;
            f0 = map->fileFrameAt(f0);
            f1 = map->fileFrameAt(f1);
            if (f0 > f1) std::swap(f0, f1);
            end = available;
        }
        const int b0 = int(std::floor(f0 * scale));
        const int b1 = std::min(end, std::max(b0 + 1, int(std::ceil(f1 * scale))));
        int peak = 0;
        for (int b = std::max(0, b0); b < b1; ++b) peak = std::max<int>(peak, lv[b]);
        if (peak == 0) continue;
        double db = Keys::valueAt(c, AnimParam::Volume, std::clamp(t, 0.0, double(c.length() - 1)));
        if (db <= kMinVolumeDb) continue;
        if (autoCurve && !autoCurve->isEmpty()) { // the track's automation plays on top (the fader does not count)
            const double a = Automation::valueAt(*autoCurve, c.start + t);
            if (a <= Automation::kMinDb + 1e-9) continue;
            db += a;
        }
        const double g = TimelineOps::audioFadeGain(c, t);
        if (g <= 0.001) continue;
        db += 20.0 * std::log10(g);
        const double v = std::min(1.0, dbTable[peak] + db / 48.0);
        if (v <= 0) continue;
        const int h = std::max(1, int(v * half));
        lines << QLine(x, mid - h, x, mid + h);
    }
    p.setPen(Theme::alpha(color, 220));
    p.drawLines(lines);
}

void StudioTracks::drawVolumeLine(QPainter& p, const QRect& body, const Clip& c)
{
    if (c.isTitle()) return;
    const double clipX = xOf(c.start);
    const auto yOf = [&](double db) { return body.bottom() - VolumeDraw::posOf(db) * (body.height() - 1); };
    // Only the part being drawn, a few pixels more (the line's width, the dots' radius)
    const int x0 = std::max({body.left(), kHeaderW, m_dirty.left() - 4}), x1 = std::min({body.right(), width() - 1, m_dirty.right() + 4});
    if (x1 < x0) return;
    QPainterPath path;
    for (int x = x0; x <= x1; ++x) {
        const double t = std::clamp((x + 0.5 - clipX) / m_view.pxPerFrame, 0.0, double(c.length() - 1));
        const QPointF pt(x, yOf(Keys::valueAt(c, AnimParam::Volume, t)));
        if (x == x0) path.moveTo(pt);
        else path.lineTo(pt);
    }
    p.setRenderHint(QPainter::Antialiasing);
    // The pen writes the track's curve: the clips' lines step back
    const QColor line = m_penTarget == PenTarget::Track ? Theme::alpha(Theme::primary, 110) : Theme::primary;
    p.setPen(QPen(line, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    // Keyframes as small dots (dense curves of the pen: only where there is room); keys of a trimmed clip outside
    // it are not shown
    const auto it = c.keys.constFind(AnimParam::Volume);
    if (keysShown(c)) {
        p.setBrush(line);
        p.setPen(QPen(Theme::border, 1));
        for (const Keyframe& k : *it) {
            if (!keyInside(c, k)) continue;
            const double x = xOf(c.start + (k.frame - c.in));
            if (x < x0 - 3 || x > x1 + 3) continue;
            p.drawEllipse(QPointF(x, yOf(k.value)), 2.5, 2.5);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

void StudioTracks::drawAutomation(QPainter& p, int track)
{
    const Track& t = m_editor->project()->timeline().audio[track];
    // A flat line at the fader's level in track mode (where the pen starts from), the curve whenever there is one
    if (t.volumeAuto.isEmpty() && m_penTarget != PenTarget::Track) return;
    const QRect body = laneRect(track).adjusted(0, 4, 0, -4);
    const auto yOf = [&](double db) { return body.bottom() - VolumeDraw::posOf(db) * (body.height() - 1); };
    const int x0 = std::max({kHeaderW, m_dirty.left() - 4}), x1 = std::min({width() - 1, m_dirty.right() + 4});
    if (x1 < x0) return;
    QPainterPath path;
    for (int x = x0; x <= x1; ++x) {
        const QPointF pt(x, yOf(Automation::levelAt(t, std::max(0.0, frameAt(x + 0.5)))));
        if (x == x0) path.moveTo(pt);
        else path.lineTo(pt);
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QColor color = t.volumeAuto.isEmpty() ? Theme::alpha(Theme::secondary, 140) : Theme::secondary;
    p.setPen(QPen(color, t.volumeAuto.isEmpty() ? 1.0 : 2.0, t.volumeAuto.isEmpty() ? Qt::DashLine : Qt::SolidLine));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    if (autoShown(t)) {
        p.setBrush(Theme::secondary);
        p.setPen(QPen(Theme::border, 1));
        for (const AutoPoint& a : t.volumeAuto) {
            const double x = xOf(a.frame);
            if (x < x0 - 4 || x > x1 + 4) continue;
            p.drawRect(QRectF(x - 3, yOf(Automation::levelAt(t, a.frame)) - 3, 6, 6));
        }
    }
    p.restore();
}

bool StudioTracks::autoShown(const Track& t) const
{
    const AutoCurve& a = t.volumeAuto;
    if (a.isEmpty()) return false;
    if (a.size() == 1) return true;
    return m_view.pxPerFrame * (a.last().frame - a.first().frame) / (a.size() - 1) >= 6;
}

QPointF StudioTracks::autoPoint(int track, int index) const
{
    const Timeline& tl = m_editor->project()->timeline();
    if (track < 0 || track >= tl.audio.size()) return {};
    const Track& t = tl.audio[track];
    if (!autoShown(t) || index < 0 || index >= t.volumeAuto.size()) return {};
    const double x = xOf(t.volumeAuto[index].frame);
    if (x < kHeaderW - 3 || x > width() + 3) return {};
    const QRect body = laneRect(track).adjusted(0, 4, 0, -4);
    return {x, body.bottom() - VolumeDraw::posOf(Automation::levelAt(t, t.volumeAuto[index].frame)) * (body.height() - 1)};
}

bool StudioTracks::keyInside(const Clip& c, const Keyframe& k)
{
    const int t = k.frame - c.in;
    return t >= 0 && t < c.length();
}

bool StudioTracks::keysShown(const Clip& c) const
{
    // Room enough between the dots: counted are the keys inside the clip (a trimmed clip keeps the others)
    const auto it = c.keys.constFind(AnimParam::Volume);
    if (it == c.keys.constEnd() || it->isEmpty()) return false;
    const auto byFrame = [](const Keyframe& k, int f) { return k.frame < f; };
    const auto first = std::lower_bound(it->cbegin(), it->cend(), c.in, byFrame);
    const auto last = std::lower_bound(first, it->cend(), c.in + c.length(), byFrame);
    const auto inside = last - first;
    return inside > 0 && m_view.pxPerFrame * c.length() / inside >= 6;
}

QPointF StudioTracks::keyPoint(int clipId, int index) const
{
    const Timeline& tl = m_editor->project()->timeline();
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(tl, clipId, &ref);
    if (!c || ref.kind != TrackKind::Audio || !keysShown(*c)) return {};
    const KeyTrack& k = c->keys.value(AnimParam::Volume);
    if (index < 0 || index >= k.size() || !keyInside(*c, k[index])) return {}; // outside a trimmed clip: hidden
    const QRect body = laneRect(ref.index).adjusted(0, 4, 0, -4);
    const double x = xOf(c->start + (k[index].frame - c->in));
    if (x < kHeaderW - 3 || x > width() + 3) return {};
    return {x, body.bottom() - VolumeDraw::posOf(k[index].value) * (body.height() - 1)};
}

StudioTracks::KeyHit StudioTracks::keyAt(const QPoint& pos) const
{
    const int track = trackAt(pos.y());
    if (track < 0 || pos.x() < kHeaderW) return {};
    const Timeline& tl = m_editor->project()->timeline();
    KeyHit best;
    double bestD = 6.0; // px
    // The automation's dots first: in track mode they are what the lane is about (a clip key at the same spot loses)
    for (int i = 0; i < tl.audio[track].volumeAuto.size(); ++i) {
        const QPointF pt = autoPoint(track, i);
        const double d = std::hypot(pt.x() - pos.x(), pt.y() - pos.y());
        if (!pt.isNull() && (d < bestD || (d <= bestD && m_penTarget == PenTarget::Track))) {
            bestD = d;
            best = {-1, i, track};
        }
    }
    for (const Clip& c : tl.audio[track].clips) {
        if (c.isTitle() || !keysShown(c)) continue;
        const KeyTrack& k = c.keys.value(AnimParam::Volume);
        for (int i = 0; i < k.size(); ++i) {
            const QPointF pt = keyPoint(c.id, i);
            const double d = std::hypot(pt.x() - pos.x(), pt.y() - pos.y());
            if (!pt.isNull() && d < bestD) {
                bestD = d;
                best = {c.id, i, -1};
            }
        }
    }
    return best;
}

void StudioTracks::dragKey(const QPoint& pos)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (m_key.track >= 0) { // an automation point: between its neighbours, the level as drawn (fader + curve)
        if (m_key.track >= tl.audio.size()) return;
        const Track& t = tl.audio[m_key.track];
        AutoCurve a = t.volumeAuto;
        if (m_key.index >= a.size()) return;
        const int lo = m_key.index > 0 ? a[m_key.index - 1].frame + 1 : 0;
        const int hi = m_key.index + 1 < a.size() ? a[m_key.index + 1].frame - 1 : INT_MAX / 2;
        if (hi < lo) return;
        const QRect area = clipArea();
        const double level = std::clamp(dbAtY(m_key.track, std::clamp(pos.y(), area.top(), area.bottom())), kMinVolumeDb, kMaxVolumeDb);
        a[m_key.index].frame = std::clamp(int(std::lround(frameAt(pos.x()))), lo, hi);
        a[m_key.index].db = std::round(level * 100) / 100 - t.volumeDb;
        m_editor->setTrackAutomation(m_key.track, a, T("Automationspunkt verschieben"), m_keyMerge);
        setToolTip(formatVolumeDb(level));
        return;
    }
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(tl, m_key.clipId, &ref);
    if (!c || ref.kind != TrackKind::Audio) return;
    const KeyTrack& k = c->keys.value(AnimParam::Volume);
    if (m_key.index >= k.size()) return;
    // Between its neighbours (the order stays), inside the clip (a trimmed clip's hidden keys may lie outside)
    const int lo = std::max(0, m_key.index > 0 ? k[m_key.index - 1].frame - c->in + 1 : 0);
    const int hi = std::min(c->length() - 1, m_key.index + 1 < k.size() ? k[m_key.index + 1].frame - c->in - 1 : c->length() - 1);
    if (hi < lo) return;
    const int t = std::clamp(int(std::lround(frameAt(pos.x()))) - c->start, lo, hi);
    const QRect area = clipArea();
    const double db = std::clamp(dbAtY(ref.index, std::clamp(pos.y(), area.top(), area.bottom())), kMinVolumeDb, kMaxVolumeDb);
    const int index = m_key.index;
    m_editor->modifyClips({m_key.clipId}, T("Keyframe verschieben"), [&](Clip& clip) {
        auto it = clip.keys.find(AnimParam::Volume);
        if (it == clip.keys.end() || index >= it->size()) return;
        (*it)[index].frame = clip.in + t;
        (*it)[index].value = db;
    }, m_keyMerge);
    setToolTip(formatVolumeDb(db));
}

void StudioTracks::setLiveTake(const Voiceover::LiveTake* take, int track)
{
    const bool newLane = takeOnNewTrack();
    m_take = take;
    m_takeTrack = take ? track : -1;
    if (takeOnNewTrack() != newLane) layoutLanes(); // its temporary lane comes or goes
    update();
}

bool StudioTracks::takeOnNewTrack() const
{
    return m_take && m_takeTrack >= m_editor->project()->timeline().audio.size();
}

int StudioTracks::laneCount() const
{
    return int(m_editor->project()->timeline().audio.size()) + (takeOnNewTrack() ? 1 : 0);
}

void StudioTracks::drawNewTrackLane(QPainter& p, int track)
{
    const QRect lane = laneRect(track);
    if (lane.bottom() < kRulerH || lane.top() > height()) return;
    p.fillRect(lane, track % 2 ? Theme::trackBgAlt : Theme::trackBg);
    const QRect h = headerRect(track);
    p.fillRect(h.adjusted(0, 0, 0, -1), Theme::trackHeader);
    p.fillRect(QRect(h.left(), h.top(), 3, h.height() - 1), Theme::recording);
    p.fillRect(QRect(h.left(), h.bottom(), width(), 1), Theme::border);
    QFont f = font();
    f.setBold(true);
    f.setPointSizeF(9);
    p.setFont(f);
    p.setPen(Theme::text);
    p.drawText(QRect(h.left() + 10, h.top() + 6, h.width() - 16, 18), Qt::AlignLeft | Qt::AlignVCenter,
               trackShortName({TrackKind::Audio, track}));
    f.setBold(false);
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(Theme::textDim);
    if (h.height() >= 50)
        p.drawText(QRect(h.left() + 10, h.top() + 24, h.width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(f).elidedText(T("Neue Spur (Aufnahme)"), Qt::ElideRight, h.width() - 16));
}

void StudioTracks::contextMenuEvent(QContextMenuEvent* e)
{
    // Right click on a keyframe dot: it goes (one undo step)
    const KeyHit hit = keyAt(e->pos());
    if (!hit.valid()) return QWidget::contextMenuEvent(e);
    if (hit.track >= 0) {
        AutoCurve a = m_editor->project()->timeline().audio[hit.track].volumeAuto;
        if (hit.index < a.size()) a.remove(hit.index);
        m_editor->setTrackAutomation(hit.track, a, T("Automationspunkt löschen"));
        e->accept();
        return;
    }
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), hit.clipId);
    if (!c) return;
    const int t = c->keys.value(AnimParam::Volume).value(hit.index).frame - c->in;
    m_editor->modifyClips({hit.clipId}, T("Keyframe löschen"), [t](Clip& clip) { Keys::removeKey(clip, AnimParam::Volume, t); });
    e->accept();
}

void StudioTracks::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasFormat(EffectsLibrary::SoundMimeType)) e->acceptProposedAction();
}

void StudioTracks::dragMoveEvent(QDragMoveEvent* e)
{
    if (e->mimeData()->hasFormat(EffectsLibrary::SoundMimeType) && e->position().x() >= kHeaderW) e->acceptProposedAction();
    else e->ignore();
}

void StudioTracks::dropEvent(QDropEvent* e)
{
    if (!e->mimeData()->hasFormat(EffectsLibrary::SoundMimeType)) return;
    const int frame = std::max(0, int(std::lround(frameAt(std::max<double>(kHeaderW, e->position().x())))));
    e->acceptProposedAction();
    emit soundDropRequested(e->mimeData()->data(EffectsLibrary::SoundMimeType), frame);
}

// ---------- Input ----------

void StudioTracks::addPenPoint(const QPoint& pos)
{
    const QRect area = clipArea();
    const double x = std::clamp<double>(pos.x(), area.left(), area.right());
    const VolumeDraw::Point pt{frameAt(x), dbAtY(m_penTrack, pos.y())};
    if (!m_stroke.isEmpty() && std::abs(m_stroke.last().frame - pt.frame) < 1e-9 && m_stroke.last().db == pt.db) return;
    m_stroke << pt;
    update();
}

void StudioTracks::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    const QPoint pos = e->position().toPoint();
    if (pos.y() < kRulerH && pos.x() >= kHeaderW) { // ruler: scrub
        m_drag = Drag::Scrub;
        emit seekRequested(std::max(0, int(std::lround(frameAt(pos.x())))));
        return;
    }
    const int track = trackAt(pos.y());
    if (track < 0) return;
    const Timeline& tl = m_editor->project()->timeline();
    if (pos.x() < kHeaderW) { // header: M/S
        if (muteRect(track).contains(pos)) m_editor->setTrackMuted(track, !tl.audio[track].muted);
        else if (soloRect(track).contains(pos)) m_editor->setTrackSolo(track, !tl.audio[track].solo);
        return;
    }
    if (m_tool == Tool::Pen) {
        if (tl.audio[track].locked) return;
        m_drag = Drag::Pen;
        m_penTrack = track;
        m_stroke.clear();
        addPenPoint(pos);
        return;
    }
    // Select/Points on a dot: drag it (time and level, one undo step); Alt+click puts it at 0 dB
    if (const KeyHit hit = keyAt(pos); hit.valid() && !tl.audio[track].locked) {
        if (e->modifiers() & Qt::AltModifier) {
            resetPoint(hit);
            return;
        }
        m_drag = Drag::Key;
        m_key = hit;
        m_keyMerge = QString("studio-key-%1").arg(++m_keyDrags);
        m_editor->project()->closeMerge();
        return;
    }
    // Points on the line: a new point there, dragged right away (adding and dragging = one undo step)
    if (m_tool == Tool::Points && !tl.audio[track].locked && onLine(track, pos)) {
        m_keyMerge = QString("studio-key-%1").arg(++m_keyDrags);
        m_editor->project()->closeMerge();
        if (const KeyHit hit = addPointAt(track, pos); hit.valid()) {
            m_drag = Drag::Key;
            m_key = hit;
            return;
        }
    }
    // Select: the playhead there, the clip under it selected (with its linked partner, like a click in the timeline)
    const double frame = frameAt(pos.x());
    if (const Clip* c = clipAt(track, frame)) {
        QSet<int> ids;
        for (int id : m_editor->withLinked({c->id})) ids.insert(id);
        m_editor->selection()->set(ids);
    } else {
        m_editor->selection()->set({});
    }
    m_drag = Drag::Scrub;
    emit seekRequested(std::max(0, int(std::lround(frame))));
}

void StudioTracks::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    m_hover = pos;
    if (m_drag == Drag::Scrub) {
        emit seekRequested(std::max(0, int(std::lround(frameAt(std::max(kHeaderW, pos.x()))))));
    } else if (m_drag == Drag::Pen) {
        addPenPoint(pos);
    } else if (m_drag == Drag::Key) {
        dragKey(pos);
    } else {
        const int track = trackAt(pos.y());
        const bool overButton = track >= 0 && (muteRect(track).contains(pos) || soloRect(track).contains(pos));
        const bool overKey = m_tool != Tool::Pen && !overButton && keyAt(pos).valid();
        const bool overLine = m_tool == Tool::Points && !overButton && !overKey && track >= 0 && pos.x() >= kHeaderW
                              && onLine(track, pos);
        setCursor(overButton || overLine                          ? Qt::PointingHandCursor
                  : overKey                                       ? Qt::SizeAllCursor
                  : m_tool == Tool::Pen && pos.x() >= kHeaderW ? Qt::CrossCursor
                                                                  : Qt::ArrowCursor);
        if (m_tool == Tool::Pen && track >= 0 && pos.x() >= kHeaderW)
            setToolTip(formatVolumeDb(dbAtY(track, pos.y())));
        else if (overKey)
            setToolTip(T("Keyframe: ziehen = verschieben, Rechtsklick = löschen"));
        else if (overLine)
            setToolTip(T("Klick setzt einen Punkt"));
        else
            setToolTip({});
    }
}

void StudioTracks::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
    if (m_drag == Drag::Pen) {
        addPenPoint(e->position().toPoint());
        const QVector<VolumeDraw::Point> stroke = std::exchange(m_stroke, {});
        m_drag = Drag::None;
        // One undo step per stroke; a click without dragging (or a stroke straight up and down) changes nothing
        if (VolumeDraw::spansEnough(stroke)) {
            if (m_penTarget == PenTarget::Track) m_editor->drawTrackVolume(m_penTrack, stroke, m_smoothing);
            else m_editor->drawVolume(m_penTrack, stroke, m_smoothing);
        }
        update();
        return;
    }
    if (m_drag == Drag::Key) {
        m_editor->project()->closeMerge();
        m_key = {};
    }
    m_drag = Drag::None;
}

void StudioTracks::mouseDoubleClickEvent(QMouseEvent* e)
{
    // Double click on a point: 0 dB (the press before it may have just set the point - then it is that one)
    if (e->button() != Qt::LeftButton || m_tool == Tool::Pen) return QWidget::mouseDoubleClickEvent(e);
    const KeyHit hit = keyAt(e->position().toPoint());
    if (!hit.valid()) return;
    m_drag = Drag::None;
    m_key = {};
    m_editor->project()->closeMerge();
    resetPoint(hit);
}

bool StudioTracks::onLine(int track, const QPoint& pos) const
{
    const Timeline& tl = m_editor->project()->timeline();
    if (track < 0 || track >= tl.audio.size() || pos.x() < kHeaderW) return false;
    const QRect body = laneRect(track).adjusted(0, 4, 0, -4);
    const auto yOf = [&](double db) { return body.bottom() - VolumeDraw::posOf(db) * (body.height() - 1); };
    const double frame = std::max(0.0, frameAt(pos.x()));
    double level;
    if (m_penTarget == PenTarget::Track) {
        level = Automation::levelAt(tl.audio[track], frame);
    } else {
        const Clip* c = clipAt(track, frame);
        if (!c || c->isTitle()) return false;
        level = Keys::valueAt(*c, AnimParam::Volume, std::clamp(frame - c->start, 0.0, double(c->length() - 1)));
    }
    return std::abs(pos.y() - yOf(level)) <= 5;
}

StudioTracks::KeyHit StudioTracks::addPointAt(int track, const QPoint& pos)
{
    const Timeline& tl = m_editor->project()->timeline();
    const int frame = std::max(0, int(std::lround(frameAt(pos.x()))));
    if (m_penTarget == PenTarget::Track) {
        AutoCurve a = tl.audio[track].volumeAuto;
        const double v = Automation::valueAt(a, frame); // on the line: the level stays until it is dragged
        a << AutoPoint{frame, v};
        m_editor->setTrackAutomation(track, Automation::normalized(a), T("Automationspunkt setzen"), m_keyMerge); // a point there already: that one
        const AutoCurve& now = m_editor->project()->timeline().audio[track].volumeAuto;
        for (int i = 0; i < now.size(); ++i)
            if (now[i].frame == frame) return {-1, i, track};
        return {};
    }
    const Clip* c = clipAt(track, frame);
    if (!c || c->isTitle()) return {};
    const int id = c->id, t = std::clamp(frame - c->start, 0, c->length() - 1);
    const double v = Keys::valueAt(*c, AnimParam::Volume, t);
    m_editor->modifyClips({id}, T("Keyframe setzen"), [&](Clip& clip) { Keys::setKey(clip, AnimParam::Volume, t, v); }, m_keyMerge);
    const Clip* now = TimelineOps::findClip(m_editor->project()->timeline(), id);
    if (!now) return {};
    const KeyTrack& k = now->keys.value(AnimParam::Volume);
    for (int i = 0; i < k.size(); ++i)
        if (k[i].frame == now->in + t) return {id, i, -1};
    return {};
}

void StudioTracks::resetPoint(const KeyHit& hit)
{
    const Timeline& tl = m_editor->project()->timeline();
    if (hit.track >= 0) {
        if (hit.track >= tl.audio.size() || tl.audio[hit.track].locked) return;
        AutoCurve a = tl.audio[hit.track].volumeAuto;
        if (hit.index >= a.size()) return;
        a[hit.index].db = -tl.audio[hit.track].volumeDb; // fader + curve = 0 dB
        m_editor->setTrackAutomation(hit.track, a, T("Automationspunkt auf 0 dB"));
        return;
    }
    const int index = hit.index;
    m_editor->modifyClips({hit.clipId}, T("Keyframe auf 0 dB"), [index](Clip& clip) {
        auto it = clip.keys.find(AnimParam::Volume);
        if (it != clip.keys.end() && index < it->size()) (*it)[index].value = 0;
    });
}

void StudioTracks::leaveEvent(QEvent* e)
{
    m_hover = {-1, -1};
    QWidget::leaveEvent(e);
}

void StudioTracks::wheelEvent(QWheelEvent* e)
{
    // The timeline's wheel settings; without room to scroll vertically a vertical wheel scrolls in time
    const QPoint delta = e->angleDelta();
    const int steps = delta.y() != 0 ? delta.y() : delta.x();
    if (steps == 0) return;
    WheelAction action = InputBindings::instance().wheelAction(e->modifiers());
    // A sideways wheel / touchpad scrolls in time - but Qt swaps the axes while Alt is held (Alt+wheel is a zoom or scroll
    // binding like on the timeline), so only without Alt
    if (delta.y() == 0 && delta.x() != 0 && !(e->modifiers() & Qt::AltModifier)) action = WheelAction::ScrollHorizontal;
    if (action == WheelAction::ScrollVertical && !m_vbar->isVisible()) action = WheelAction::ScrollHorizontal;
    switch (action) {
    case WheelAction::Zoom: {
        const double x = std::max<double>(kHeaderW, e->position().x());
        const double anchor = frameAt(x);
        m_view.setZoomAround(m_view.pxPerFrame * std::pow(1.0015, steps), anchor, x - kHeaderW);
        break;
    }
    case WheelAction::ScrollHorizontal: m_view.scrollFrames(-steps / 120.0 * clipArea().width() / 8 / m_view.pxPerFrame); break;
    case WheelAction::ScrollVertical:
        m_scrollY = std::clamp(m_scrollY - int(std::lround(steps / 120.0 * m_laneH / 3)), 0, m_vbar->maximum());
        break;
    default: return;
    }
    syncScrollBars();
    update();
    e->accept();
}
