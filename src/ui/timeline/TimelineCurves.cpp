// Kurven-Editor der Timeline (wie DaVincis Kurven unter dem Clip): Zeichnen und Maus.
// Die Kurve zeigt einen animierten Parameter über die Cliplänge; Punkte = Keyframes, bei Bezier mit Griffen.
#include "ui/timeline/TimelineView.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Timecode.h"

#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kSnapPx = 8;
constexpr int kGrabPx = 6;      // so nah an einem Punkt/Griff wird er gegriffen
constexpr int kChipH = 15;      // Parameter-Auswahl oben links
constexpr int kPlotTop = 21;    // Abstand der Kurvenfläche vom oberen Rand
constexpr int kPlotBottom = 8;

bool curveable(AnimParam p) { return !Keys::info(p).color; } // Farben haben keine Kurve

// Erster Parameter mit Kurve (Reihenfolge wie im Inspector)
std::optional<AnimParam> firstCurveParam(const Clip& c)
{
    for (auto it = c.keys.cbegin(); it != c.keys.cend(); ++it)
        if (!it->isEmpty() && curveable(it.key())) return it.key();
    return std::nullopt;
}

QString formatValue(double v, double span)
{
    const int decimals = span < 10 ? 2 : span < 100 ? 1 : 0;
    return QLocale().toString(v, 'f', decimals);
}

// Bereich des Parameters einhalten (Lautstärke, Deckkraft …)
double clampValue(AnimParam p, double v)
{
    double lo, hi;
    Keys::range(p, &lo, &hi);
    return lo < hi ? std::clamp(v, lo, hi) : v;
}

} // namespace

void TimelineView::setCurveEditor(const QVector<int>& ids, bool on, AnimParam param)
{
    const Timeline& tl = m_editor->project()->timeline();
    for (int id : ids) {
        const Clip* c = TimelineOps::findClip(tl, id);
        const auto first = c ? firstCurveParam(*c) : std::nullopt;
        if (on && first) m_curves.insert(id, param != AnimParam::Count && Keys::animated(*c, param) ? param : *first);
        else m_curves.remove(id);
    }
    Selection* sel = m_editor->selection();
    if (ids.contains(sel->keyClip()) && sel->keyParam() >= 0) sel->setKeyframes(0, {});
    setScrollY(m_view.scrollY); // Höhe hat sich geändert
}

bool TimelineView::curveEditorShown(int clipId) const
{
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), clipId);
    return c && curveParam(*c).has_value();
}

std::optional<AnimParam> TimelineView::curveParam(const Clip& c) const
{
    const auto it = m_curves.constFind(c.id);
    if (it == m_curves.cend()) return std::nullopt;
    if (Keys::animated(c, *it) && curveable(*it)) return *it;
    return firstCurveParam(c); // gewählter Parameter hat keine Keyframes mehr
}

QRect TimelineView::curveIconRect(const QRect& r) const
{
    return QRect(r.right() - 29, r.bottom() - 12, 13, 11);
}

int TimelineView::curveIconAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return 0;
    const auto row = rowAt(pos.y());
    if (!row) return 0;
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const QRect r = clipRect(*row, c);
        if (r.width() >= 56 && r.height() >= 24 && firstCurveParam(c) &&
            curveIconRect(r).adjusted(-2, -2, 1, 2).contains(pos))
            return c.id;
    }
    return 0;
}

void TimelineView::drawCurveIcon(QPainter& p, const QRect& r, bool open)
{
    const QRectF box = curveIconRect(r);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(0, 0, 0, 170), 1));
    p.setBrush(open ? Theme::primary : Theme::secondary);
    p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
    // kleine S-Kurve
    QPainterPath s;
    s.moveTo(box.left() + 2.5, box.bottom() - 2.5);
    s.cubicTo(box.center().x() + 1, box.bottom() - 2.5, box.center().x() - 1, box.top() + 2.5, box.right() - 2.5,
              box.top() + 2.5);
    p.setPen(QPen(Theme::readableOn(open ? Theme::primary : Theme::secondary), 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawPath(s);
    p.restore();
}

QRect TimelineView::curveRect(const Row& row, const Clip& c) const
{
    const int top = keyLaneTop(row) + row.keyLane;
    return QRect(QPoint(int(frameToX(c.start)), top), QPoint(int(frameToX(c.end())) - 1, row.y + row.h - 3));
}

QRect TimelineView::curvePlotRect(const QRect& curve) const
{
    return curve.adjusted(0, kPlotTop, 0, -kPlotBottom);
}

QRect TimelineView::curveChipRect(const QRect& curve, const Clip& c) const
{
    const auto param = curveParam(c);
    QFont f = font();
    f.setPointSizeF(8);
    const int w = QFontMetrics(f).horizontalAdvance((param ? Keys::label(*param) : QString()) + "  ▾") + 12;
    return QRect(std::max(curve.left(), kHeaderW) + 4, curve.top() + 3, w, kChipH);
}

TimelineView::CurveRange TimelineView::curveRange(const Clip& c, AnimParam p) const
{
    // beim Ziehen eingefroren, sonst springt die Skala unter der Maus
    if (m_curveFreeze && m_curveDrag.clipId == c.id && m_curveDrag.param == p) return m_curveFrozen;
    const KeyTrack k = c.keys.value(p);
    double lo = Keys::valueAt(c, p, 0), hi = lo;
    auto add = [&](double v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };
    for (int i = 0; i < k.size(); ++i) {
        add(k[i].value);
        for (bool out : {false, true}) {
            double dt, dv;
            if (Keys::handle(k, i, out, &dt, &dv)) add(k[i].value + dv);
        }
    }
    const double span = hi - lo;
    if (span < 1e-9) {
        const double pad = std::max(std::abs(lo) * 0.1, 1.0);
        return {lo - pad, hi + pad};
    }
    return {lo - span * 0.12, hi + span * 0.12};
}

double TimelineView::curveY(const QRect& plot, const CurveRange& r, double v) const
{
    return plot.bottom() - (v - r.lo) / (r.hi - r.lo) * plot.height();
}

double TimelineView::curveValue(const QRect& plot, const CurveRange& r, double y) const
{
    return r.lo + (plot.bottom() - y) / double(plot.height()) * (r.hi - r.lo);
}

std::optional<TimelineView::CurveHit> TimelineView::curveHitAt(const QPoint& pos) const
{
    if (pos.x() < kHeaderW || pos.y() < kRulerH) return std::nullopt;
    const auto row = rowAt(pos.y());
    if (!row || !row->curve || isLocked(*row)) return std::nullopt;
    const Selection* sel = m_editor->selection();
    for (const Clip& c : m_editor->project()->timeline().track(row->ref).clips) {
        const auto param = curveParam(c);
        if (!param) continue;
        const QRect R = curveRect(*row, c);
        if (!R.adjusted(-kGrabPx, 0, kGrabPx, 0).contains(pos)) continue;
        if (curveChipRect(R, c).contains(pos)) return CurveHit{c.id, *param, -1, 2};
        const QRect plot = curvePlotRect(R);
        const CurveRange range = curveRange(c, *param);
        const KeyTrack k = c.keys.value(*param);
        const bool mine = sel->keyClip() == c.id && sel->keyParam() == int(*param);
        auto dist = [&](double x, double y) { return std::hypot(x - pos.x(), y - pos.y()); };
        // Griffe der ausgewählten Punkte liegen über den Punkten
        for (int i = 0; i < k.size(); ++i) {
            const int t = k[i].frame - c.in;
            if (!mine || !sel->keyTimes().contains(t) || t < 0 || t >= c.length()) continue;
            for (bool out : {false, true}) {
                double dt, dv;
                if (!Keys::handle(k, i, out, &dt, &dv) || (dt == 0 && dv == 0)) continue;
                if (dist(frameToX(c.start + t + dt), curveY(plot, range, k[i].value + dv)) <= kGrabPx)
                    return CurveHit{c.id, *param, i, out ? 1 : -1};
            }
        }
        int best = -1;
        double bestDist = kGrabPx + 1;
        for (int i = 0; i < k.size(); ++i) {
            const int t = k[i].frame - c.in;
            if (t < 0 || t >= c.length()) continue;
            const double d = dist(frameToX(c.start + t), curveY(plot, range, k[i].value));
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        if (R.contains(pos) || best >= 0) return CurveHit{c.id, *param, best, 0};
    }
    return std::nullopt;
}

bool TimelineView::inCurve(const QPoint& pos, int* clipId) const
{
    const auto h = curveHitAt(pos);
    if (h && clipId) *clipId = h->clipId;
    return h.has_value();
}

int TimelineView::keySnapDelta(const Clip& c, const QVector<int>& times, const QSet<int>& exclude) const
{
    if (!m_snap || times.isEmpty()) return 0;
    QVector<int> targets{m_playhead - c.start, 0, c.length() - 1};
    for (int t : Keys::keyTimes(c))
        if (!exclude.contains(t)) targets << t;
    const double maxFrames = kSnapPx / m_view.pxPerFrame;
    int best = 0;
    double bestDist = maxFrames + 1;
    for (int target : targets)
        for (int t : times) {
            const double d = std::abs(target - t);
            if (d <= maxFrames && d < bestDist) {
                bestDist = d;
                best = target - t;
            }
        }
    return best;
}

void TimelineView::drawCurveLane(QPainter& p, const Row& row, const Clip& c)
{
    const auto param = curveParam(c);
    const QRect R = curveRect(row, c);
    if (!param || R.right() < kHeaderW || R.left() > width()) return;
    const Selection* sel = m_editor->selection();
    const bool mine = sel->keyClip() == c.id && sel->keyParam() == int(*param);
    const QRect plot = curvePlotRect(R);
    const CurveRange range = curveRange(c, *param);
    const KeyTrack k = c.keys.value(*param);

    p.save();
    p.fillRect(R, Theme::lane);
    p.setPen(Theme::control);
    p.drawRect(R.adjusted(0, 0, -1, -1));
    p.setClipRect(R.adjusted(-8, 0, 8, 0).intersected(QRect(kHeaderW, 0, width() - kHeaderW, height())),
                  Qt::IntersectClip);

    // Rasterlinien mit Werten (oben, Mitte, unten)
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    const int labelX = std::max(R.left(), kHeaderW) + 6;
    for (double frac : {1.0, 0.5, 0.0}) {
        const double v = range.lo + (range.hi - range.lo) * frac;
        const int y = int(std::lround(curveY(plot, range, v)));
        p.setPen(QPen(Theme::alpha(Theme::text, 28), 1, Qt::DashLine));
        p.drawLine(R.left() + 1, y, R.right() - 1, y);
        // Wert über der Linie, oben darunter (darüber liegt die Parameter-Auswahl)
        p.setPen(Theme::textDim);
        const QRect box = frac == 1.0 ? QRect(labelX, y + 1, 90, 12) : QRect(labelX, y - 13, 90, 12);
        p.drawText(box, Qt::AlignLeft | (frac == 1.0 ? Qt::AlignTop : Qt::AlignBottom), formatValue(v, range.hi - range.lo));
    }

    // Kurve (nur sichtbarer Teil, alle 2 px)
    p.setRenderHint(QPainter::Antialiasing);
    const int x0 = std::max(R.left(), kHeaderW), x1 = std::min(R.right(), width());
    QPainterPath path;
    for (int x = x0; x <= x1 + 1; x += 2) {
        const double t = std::clamp((std::min(x, x1) - frameToX(c.start)) / m_view.pxPerFrame, 0.0,
                                    double(c.length() - 1));
        const QPointF pt(std::min(x, x1), curveY(plot, range, Keys::valueAt(c, *param, t)));
        if (x == x0) path.moveTo(pt); // nicht path.isEmpty(): ist nach moveTo noch true
        else path.lineTo(pt);
    }
    p.setPen(QPen(Theme::mix(Theme::primary, Theme::meterMid, 0.25), 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    // Punkte; ausgewählte orange mit Griffen
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < k.size(); ++i) {
            const int t = k[i].frame - c.in;
            if (t < 0 || t >= c.length()) continue;
            const bool selected = mine && sel->keyTimes().contains(t);
            if (selected != (pass == 1)) continue;
            const QPointF pt(frameToX(c.start + t), curveY(plot, range, k[i].value));
            if (selected)
                for (bool out : {false, true}) {
                    double dt, dv;
                    if (!Keys::handle(k, i, out, &dt, &dv) || (dt == 0 && dv == 0)) continue;
                    const QPointF h(frameToX(c.start + t + dt), curveY(plot, range, k[i].value + dv));
                    p.setPen(QPen(Theme::textDim, 1));
                    p.drawLine(pt, h);
                    p.setBrush(k[i].ease == KeyEase::Bezier ? Theme::secondary : Theme::controlOff);
                    p.drawRect(QRectF(h.x() - 2.5, h.y() - 2.5, 5, 5));
                }
            p.setPen(QPen(QColor(0, 0, 0, 180), 1));
            p.setBrush(selected ? Theme::primary : Theme::secondary);
            p.drawEllipse(pt, 3.8, 3.8);
        }

    // Parameter-Auswahl oben links (Klick = Menü)
    const QRect chip = curveChipRect(R, c);
    f.setPointSizeF(8);
    p.setFont(f);
    p.setPen(Theme::controlLight);
    p.setBrush(Theme::ruler);
    p.drawRoundedRect(QRectF(chip).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    p.setPen(Theme::text);
    p.drawText(chip.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, Keys::label(*param) + "  ▾");
    p.restore();

    // Beim Ziehen: Wert und Zeitversatz am gegriffenen Punkt
    if (m_drag == Drag::CurvePoint && m_curveDrag.clipId == c.id && m_curveDrag.index >= 0 &&
        m_curveDrag.index < m_curveOrig.size()) {
        const Keyframe& o = m_curveOrig[m_curveDrag.index];
        const int t = o.frame - c.in + m_curveDragDt;
        const double v = Keys::valueAt(c, *param, t);
        QString text = formatValue(v, range.hi - range.lo);
        if (m_curveDragDt != 0)
            text += "   " + QString(m_curveDragDt > 0 ? "+" : "") +
                    Timecode::format(m_curveDragDt, m_editor->project()->fps());
        drawLabel(p, QPoint(int(frameToX(c.start + t)) + 10, int(curveY(plot, range, v)) - 24), text);
    }
}

void TimelineView::curveParamMenu(int clipId, const QPoint& globalPos)
{
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), clipId);
    if (!c) return;
    const auto current = curveParam(*c);
    QMenu menu(this);
    for (auto it = c->keys.cbegin(); it != c->keys.cend(); ++it) {
        if (it->isEmpty() || !curveable(it.key())) continue;
        QAction* a = menu.addAction(Keys::label(it.key()));
        a->setCheckable(true);
        a->setChecked(current == it.key());
        connect(a, &QAction::triggered, this, [this, clipId, p = it.key()] {
            m_curves[clipId] = p;
            Selection* sel = m_editor->selection();
            if (sel->keyClip() == clipId && sel->keyParam() >= 0) sel->setKeyframes(0, {});
            update();
        });
    }
    menu.exec(globalPos);
}

bool TimelineView::curvePress(QMouseEvent* e, const QPoint& pos)
{
    // Kurven-Symbol im Clip klappt den Kurven-Editor auf/zu
    if (const int id = curveIconAt(pos)) {
        if (m_curves.contains(id)) {
            m_curves.remove(id);
        } else if (const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), id)) {
            m_curves.insert(id, *firstCurveParam(*c));
        }
        Selection* sel = m_editor->selection();
        if (sel->keyClip() == id && sel->keyParam() >= 0) sel->setKeyframes(0, {});
        setScrollY(m_view.scrollY); // Höhe hat sich geändert
        return true;
    }
    const auto h = curveHitAt(pos);
    if (!h) return false;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), h->clipId);
    if (!c) return true;
    Selection* sel = m_editor->selection();
    if (!sel->contains(h->clipId)) {
        const QVector<int> group = m_editor->withLinked({h->clipId});
        sel->set(QSet<int>(group.begin(), group.end()));
    }
    if (h->part == 2) {
        const QRect chip = curveChipRect(curveRect(*rowAt(pos.y()), *c), *c);
        curveParamMenu(h->clipId, mapToGlobal(chip.bottomLeft() + QPoint(0, 2)));
        return true;
    }
    if (h->index < 0) { // leere Stelle: Punkte abwählen
        if (sel->keyParam() >= 0) sel->setKeyframes(0, {});
        update();
        return true;
    }
    const KeyTrack k = c->keys.value(h->param);
    const int t = k[h->index].frame - c->in;
    if (h->part == 0) {
        QSet<int> times = sel->keyClip() == h->clipId && sel->keyParam() == int(h->param) ? sel->keyTimes() : QSet<int>{};
        const bool ctrl = e->modifiers() & Qt::ControlModifier; // Shift = eine Achse beim Ziehen
        if (ctrl && times.contains(t)) times.remove(t);
        else if (ctrl) times.insert(t);
        else if (!times.contains(t)) times = {t};
        sel->setKeyframes(h->clipId, times, int(h->param));
        if (ctrl) {
            update();
            return true;
        }
        emit seekRequested(c->start + t); // Inspector zeigt den Wert an diesem Keyframe
    }
    m_curveDrag = *h;
    m_curveOrig = k;
    m_curveOrigTimes = sel->keyTimes();
    m_curveFreeze = false;
    m_curveFrozen = curveRange(*c, h->param);
    m_curveFreeze = true;
    m_curveDragDt = 0;
    m_curveDragDv = 0;
    m_drag = h->part == 0 ? Drag::CurvePoint : Drag::CurveHandle;
    update();
    return true;
}

void TimelineView::curveMove(const QPoint& pos, Qt::KeyboardModifiers mods)
{
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_curveDrag.clipId, &ref);
    const auto row = c ? rowFor(ref) : std::nullopt;
    if (!c || !row || m_curveDrag.index < 0 || m_curveDrag.index >= m_curveOrig.size()) return;
    const QRect plot = curvePlotRect(curveRect(*row, *c));
    const CurveRange& range = m_curveFrozen;
    const AnimParam param = m_curveDrag.param;
    const Keyframe& grabbed = m_curveOrig[m_curveDrag.index];
    KeyTrack k = m_curveOrig;

    if (m_drag == Drag::CurveHandle) {
        const double dt = xToFrame(pos.x()) - (c->start + grabbed.frame - c->in);
        const double dv = curveValue(plot, range, pos.y()) - grabbed.value;
        Keys::setHandle(k, m_curveDrag.index, m_curveDrag.part > 0, dt, dv, mods & Qt::AltModifier);
    } else {
        const double dx = pos.x() - m_pressPos.x(), dy = pos.y() - m_pressPos.y();
        int delta = int(std::lround(dx / m_view.pxPerFrame));
        double dv = -dy / plot.height() * (range.hi - range.lo);
        if (mods & Qt::ShiftModifier) { // eine Achse (wie DaVinci)
            if (std::abs(dx) > std::abs(dy)) dv = 0;
            else delta = 0;
        }
        const QSet<int>& times = m_curveOrigTimes;
        if (times.isEmpty()) return;
        if (delta != 0) delta += keySnapDelta(*c, {grabbed.frame - c->in + delta}, times);
        const int lo = *std::min_element(times.begin(), times.end());
        const int hi = *std::max_element(times.begin(), times.end());
        delta = std::clamp(delta, -lo, c->length() - 1 - hi);
        KeyTrack moved, kept;
        for (Keyframe key : m_curveOrig) {
            if (times.contains(key.frame - c->in)) {
                key.frame += delta;
                key.value = clampValue(param, key.value + dv);
                moved << key;
            } else {
                kept << key;
            }
        }
        // verschobene Punkte gewinnen gegen solche, die am Ziel schon liegen
        kept.erase(std::remove_if(kept.begin(), kept.end(), [&](const Keyframe& x) {
            return std::any_of(moved.begin(), moved.end(), [&](const Keyframe& m) { return m.frame == x.frame; });
        }), kept.end());
        k = kept + moved;
        std::sort(k.begin(), k.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
        m_curveDragDt = delta;
        m_curveDragDv = dv;
        QSet<int> shifted;
        for (int t : times) shifted.insert(t + delta);
        m_editor->selection()->setKeyframes(m_curveDrag.clipId, shifted, int(param));
    }
    if (c->keys.value(param) == k) {
        update();
        return;
    }
    // live wie beim Fade-Ziehen: ein Undo-Schritt pro Ziehen
    m_editor->modifyClips({m_curveDrag.clipId}, T("Kurve bearbeiten"), [&](Clip& clip) { clip.keys[param] = k; },
                          QStringLiteral("curve-edit"));
    update();
}

void TimelineView::curveRelease()
{
    m_editor->project()->closeMerge();
    if (m_drag == Drag::CurvePoint && m_curveDragDt != 0 && m_curveDrag.index < m_curveOrig.size())
        if (const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), m_curveDrag.clipId))
            emit seekRequested(c->start + m_curveOrig[m_curveDrag.index].frame - c->in + m_curveDragDt);
    m_curveFreeze = false;
    m_curveOrig.clear();
    m_curveOrigTimes.clear();
    m_curveDragDt = 0;
    m_curveDragDv = 0;
}

bool TimelineView::curveContextMenu(const QPoint& pos, const QPoint& globalPos)
{
    const auto h = curveHitAt(pos);
    if (!h) return false;
    if (h->part != 0 || h->index < 0) {
        curveParamMenu(h->clipId, globalPos);
        return true;
    }
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), h->clipId);
    if (!c) return true;
    const KeyTrack k = c->keys.value(h->param);
    const int t = k[h->index].frame - c->in;
    Selection* sel = m_editor->selection();
    if (sel->keyClip() != h->clipId || sel->keyParam() != int(h->param) || !sel->keyTimes().contains(t))
        sel->setKeyframes(h->clipId, {t}, int(h->param));
    const QVector<int> times = sel->keyTimes().values().toVector();
    const int id = h->clipId;
    const AnimParam param = h->param;

    QMenu menu(this);
    const struct { KeyEase ease; const char* name; } eases[] = {
        {KeyEase::Linear, "Linear"}, {KeyEase::EaseIn, "Ease In"}, {KeyEase::EaseOut, "Ease Out"},
        {KeyEase::EaseInOut, "Ease In and Out"}, {KeyEase::Bezier, "Bezier"}};
    for (const auto& it : eases) {
        QAction* a = menu.addAction(it.name); // wie DaVinci auch deutsch englisch
        a->setCheckable(true);
        a->setChecked(k[h->index].ease == it.ease);
        connect(a, &QAction::triggered, this, [this, id, times, param, ease = it.ease] {
            m_editor->modifyClips({id}, T("Keyframe-Verlauf"),
                                  [&](Clip& clip) { Keys::setEase(clip, times, ease, {param}); });
        });
    }
    connect(menu.addAction(T("Weiche Griffe")), &QAction::triggered, this, [this, id, times, param] {
        m_editor->modifyClips({id}, T("Keyframe-Verlauf"), [&](Clip& clip) {
            KeyTrack& track = clip.keys[param];
            for (int i = 0; i < track.size(); ++i)
                if (times.contains(track[i].frame - clip.in)) {
                    track[i].ease = KeyEase::Bezier;
                    Keys::autoHandles(track, i);
                }
        });
    });
    menu.addSeparator();
    connect(menu.addAction(T("Löschen")), &QAction::triggered, this,
            [this, id, times, param] { m_editor->removeKeyframes(id, times, {param}); });
    menu.exec(globalPos);
    return true;
}

bool TimelineView::curveDoubleClick(const QPoint& pos)
{
    const auto h = curveHitAt(pos);
    if (!h || h->part == 2) return false;
    if (h->index >= 0) return true;
    TrackRef ref;
    const Clip* c = TimelineOps::findClip(m_editor->project()->timeline(), h->clipId, &ref);
    const auto row = c ? rowFor(ref) : std::nullopt;
    if (!c || !row) return true;
    // Doppelklick in die Kurvenfläche: Keyframe an dieser Stelle mit dem Wert unter der Maus
    const QRect plot = curvePlotRect(curveRect(*row, *c));
    const int t = std::clamp(int(std::lround(xToFrame(pos.x()) - c->start)), 0, c->length() - 1);
    const double v = clampValue(h->param, curveValue(plot, curveRange(*c, h->param), pos.y()));
    const AnimParam param = h->param;
    const int id = h->clipId;
    m_editor->modifyClips({id}, T("Keyframe setzen"), [&](Clip& clip) { Keys::setKey(clip, param, t, v); });
    m_editor->selection()->setKeyframes(id, {t}, int(param));
    update();
    return true;
}
