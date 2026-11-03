// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ReviewSheet.h"

#include "core/I18n.h"
#include "core/Timecode.h"

#include <QFontMetrics>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <optional>

namespace ReviewSheet {

namespace {

int kindOrder(MarkKind k) { return int(k); }

double frameS(const Data& d, int frame) { return frame / d.fps(); }

int fpsInt(const Data& d) { return std::max(1, int(std::lround(d.fps()))); }

QString timecode(const Data& d, int frame) { return Timecode::format(frame, fpsInt(d)); }

QString sectionTitle(const Section& s) { return s.name.isEmpty() ? s.kind : s.kind + " · " + s.name; }

const Section* sectionAt(const Data& d, int frame)
{
    for (const Section& s : d.sections)
        if (frame >= s.from && frame < s.to) return &s;
    return nullptr;
}

QFont font(double px, bool bold = false)
{
    QFont f;
    f.setPixelSize(std::max(7, int(std::lround(px))));
    f.setBold(bold);
    return f;
}

// Text with a dark shadow so it reads on any picture
void shadowText(QPainter& p, QPointF at, const QString& text, const QColor& color)
{
    p.setPen(QColor(0, 0, 0, 200));
    p.drawText(at + QPointF(1, 1), text);
    p.setPen(color);
    p.drawText(at, text);
}

void triangle(QPainter& p, QPointF tip, double size, bool down, const QColor& color, bool filled)
{
    QPainterPath path;
    const double dy = down ? -size : size;
    path.moveTo(tip);
    path.lineTo(tip.x() - size * 0.6, tip.y() + dy);
    path.lineTo(tip.x() + size * 0.6, tip.y() + dy);
    path.closeSubpath();
    if (filled) p.fillPath(path, color);
    p.setPen(QPen(filled ? QColor(0, 0, 0, 160) : color, std::max(1.0, size / 6)));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

// What a lane between seconds `from` and `to` shows inside `r`. u = pixel unit (1 at 540 p)
struct Lane {
    QRectF r;
    double from = 0, to = 1;
    double u = 1;
    double x(double s) const { return r.left() + (s - from) / (to - from) * r.width(); }
    bool inside(double s) const { return s >= from && s <= to; }
};

void drawWave(QPainter& p, const Lane& lane, const Wave& w, const QColor& color)
{
    if (w.peak.empty()) return;
    const double mid = lane.r.center().y(), half = lane.r.height() / 2;
    const int x0 = int(std::floor(lane.r.left())), x1 = int(std::ceil(lane.r.right()));
    const double perPixel = (lane.to - lane.from) / std::max(1.0, lane.r.width());
    p.setPen(QPen(color, 1));
    for (int x = x0; x < x1; ++x) {
        const double s0 = lane.from + (x - lane.r.left()) * perPixel, s1 = s0 + perPixel;
        const int i0 = std::max(0, int(std::floor(s0 * kWaveRate))), i1 = int(std::ceil(s1 * kWaveRate));
        float peak = 0;
        for (int i = i0; i < std::max(i0 + 1, i1) && i < int(w.peak.size()); ++i) peak = std::max(peak, w.peak[size_t(i)]);
        if (peak <= 0) continue;
        const double h = std::max(1.0, double(peak) * half);
        p.drawLine(QPointF(x + 0.5, mid - h), QPointF(x + 0.5, mid + h));
    }
}

// Beat lines: bar ones full height and thicker, the other beats from the top down to `beatShare` of the height
void drawBeats(QPainter& p, const Lane& lane, const Data& d, double beatShare, int alpha)
{
    const QColor c(0, 200, 255, alpha);
    for (size_t k = 0; k < d.beats.size(); ++k) {
        const double s = d.beats[k];
        if (!lane.inside(s)) continue;
        const bool bar = k < d.bar.size() && d.bar[k];
        p.setPen(QPen(c, bar ? 3 * lane.u : 1 * lane.u));
        const double x = lane.x(s);
        p.drawLine(QPointF(x, lane.r.top()), QPointF(x, bar ? lane.r.bottom() : lane.r.top() + lane.r.height() * beatShare));
    }
}

void drawSections(QPainter& p, const Lane& lane, const Data& d, const QRectF& band)
{
    p.setFont(font(10 * lane.u, true));
    for (const Section& s : d.sections) {
        const double a = std::max(lane.from, frameS(d, s.from)), b = std::min(lane.to, frameS(d, s.to));
        if (b <= a) continue;
        const QRectF box(lane.x(a), band.top(), lane.x(b) - lane.x(a), band.height());
        QColor c = sectionColor(s.kind);
        c.setAlpha(200);
        p.fillRect(box, c);
        p.setPen(QColor(0, 0, 0, 220));
        p.drawRect(box);
        p.setPen(Qt::white);
        p.drawText(box.adjusted(3 * lane.u, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                   sectionTitle(s));
    }
}

// Cuts (line + clip name above), hits ▼, kill feed lines ▽, accents ◆, notes (flag + text), markers
void drawMarks(QPainter& p, const Lane& lane, const Data& d, const QRectF& names, bool cutNames)
{
    const double u = lane.u;
    p.setFont(font(10 * u));
    for (const Mark& m : d.marks) {
        const double s = frameS(d, m.frame);
        if (!lane.inside(s)) continue;
        const double x = lane.x(s);
        const QColor c = markColor(m.kind);
        switch (m.kind) {
        case MarkKind::Cut:
            p.setPen(QPen(c, 2 * u));
            p.drawLine(QPointF(x, names.top()), QPointF(x, lane.r.bottom()));
            if (cutNames && !m.label.isEmpty()) shadowText(p, QPointF(x + 3 * u, names.bottom() - 2 * u), m.label, c);
            break;
        case MarkKind::Hit:
            triangle(p, QPointF(x, lane.r.top() + 12 * u), 11 * u, true, c, true);
            break;
        case MarkKind::Kill:
            triangle(p, QPointF(x, lane.r.top() + 26 * u), 10 * u, true, c, false);
            break;
        case MarkKind::Accent: {
            const double r = 5 * u, y = lane.r.top() + 34 * u;
            QPainterPath path;
            path.moveTo(x, y - r);
            path.lineTo(x + r, y);
            path.lineTo(x, y + r);
            path.lineTo(x - r, y);
            path.closeSubpath();
            p.fillPath(path, c);
            break;
        }
        case MarkKind::Note: {
            const double y = lane.r.bottom() - 4 * u;
            p.setPen(QPen(c, 2 * u));
            p.drawLine(QPointF(x, y), QPointF(x, y - 16 * u));
            p.fillRect(QRectF(x, y - 16 * u, 8 * u, 6 * u), c);
            if (!m.label.isEmpty()) shadowText(p, QPointF(x + 10 * u, y - 9 * u), m.label.left(40), c);
            break;
        }
        case MarkKind::Marker:
            triangle(p, QPointF(x, lane.r.bottom() - 7 * u), 7 * u, false, c, true);
            break;
        }
    }
}

} // namespace

void Data::sort()
{
    std::stable_sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) {
        return a.frame != b.frame ? a.frame < b.frame : kindOrder(a.kind) < kindOrder(b.kind);
    });
    std::stable_sort(sections.begin(), sections.end(), [](const Section& a, const Section& b) { return a.from < b.from; });
}

int nearestBeat(const Data& d, double s)
{
    if (d.beats.empty()) return -1;
    const auto it = std::lower_bound(d.beats.begin(), d.beats.end(), s);
    if (it == d.beats.begin()) return 0;
    if (it == d.beats.end()) return int(d.beats.size()) - 1;
    const int i = int(it - d.beats.begin());
    return s - d.beats[size_t(i - 1)] <= d.beats[size_t(i)] - s ? i - 1 : i;
}

namespace {

std::optional<Offset> offsetOf(const Data& d, const Mark& m)
{
    if (d.beats.empty()) return std::nullopt;
    const double period = d.beats.size() > 1 ? (d.beats.back() - d.beats.front()) / double(d.beats.size() - 1) : 0.5;
    const double s = frameS(d, m.frame);
    if (s < d.beats.front() - period || s > d.beats.back() + period) return std::nullopt; // outside the music
    const int k = nearestBeat(d, s);
    Offset o;
    o.frame = m.frame;
    o.at = s;
    o.beat = d.beats[size_t(k)];
    o.offMs = int(std::lround((s - o.beat) * 1000));
    o.clip = m.clip;
    o.label = m.label;
    return o;
}

} // namespace

QVector<Offset> offsets(const Data& d, MarkKind kind)
{
    QVector<Offset> out;
    for (const Mark& m : d.marks)
        if (m.kind == kind)
            if (const std::optional<Offset> o = offsetOf(d, m)) out << *o;
    return out;
}

QVector<Offset> cutsOffBeat(const Data& d)
{
    QVector<Offset> out;
    const double half = 500.0 / d.fps(); // ms
    for (const Offset& o : offsets(d, MarkKind::Cut))
        if (std::abs(o.offMs) > half + 0.5) out << o;
    return out;
}

// ---- Lamps

QColor lampColor(Lamp l)
{
    switch (l) {
    case Lamp::Beat: return QColor(0, 200, 255);
    case Lamp::Bar: return QColor(255, 255, 255);
    case Lamp::Cut: return markColor(MarkKind::Cut);
    case Lamp::Hit: return markColor(MarkKind::Hit);
    case Lamp::Kill: return markColor(MarkKind::Kill);
    case Lamp::Count: break;
    }
    return Qt::gray;
}

QString lampName(Lamp l)
{
    switch (l) {
    case Lamp::Beat: return T("Beat");
    case Lamp::Bar: return T("Takt");
    case Lamp::Cut: return T("Schnitt");
    case Lamp::Hit: return T("Treffer");
    case Lamp::Kill: return T("Kill");
    case Lamp::Count: break;
    }
    return {};
}

int lampHold(double fps) { return std::max(1, int(std::lround(fps * 0.1))); }

bool lampLit(const Data& d, Lamp l, int frame)
{
    const int hold = lampHold(d.fps());
    const auto litAt = [&](int at) { return frame >= at && frame < at + hold; };
    if (l == Lamp::Beat || l == Lamp::Bar) {
        for (size_t k = 0; k < d.beats.size(); ++k) {
            if (l == Lamp::Bar && !(k < d.bar.size() && d.bar[k])) continue;
            if (litAt(int(std::lround(d.beats[k] * d.fps())))) return true;
        }
        return false;
    }
    const MarkKind kind = l == Lamp::Cut ? MarkKind::Cut : l == Lamp::Hit ? MarkKind::Hit : MarkKind::Kill;
    for (const Mark& m : d.marks)
        if (m.kind == kind && litAt(m.frame)) return true;
    return false;
}

QRect lampRect(Lamp l, QSize size)
{
    const double u = size.height() / 540.0;
    const int side = std::max(8, int(std::lround(30 * u))), gap = std::max(3, int(std::lround(8 * u)));
    const int n = int(Lamp::Count), margin = std::max(4, int(std::lround(10 * u)));
    const int x0 = size.width() - margin - n * side - (n - 1) * gap;
    return QRect(x0 + int(l) * (side + gap), margin, side, side);
}

QRect stripRect(QSize size)
{
    const int h = std::max(40, int(std::lround(size.height() * 0.2)));
    return QRect(0, size.height() - h, size.width(), h);
}

QColor markColor(MarkKind k)
{
    switch (k) {
    case MarkKind::Cut: return QColor(255, 215, 0);
    case MarkKind::Hit: return QColor(255, 40, 40);
    case MarkKind::Kill: return QColor(255, 140, 0);
    case MarkKind::Accent: return QColor(230, 70, 230);
    case MarkKind::Note: return QColor(70, 220, 100);
    case MarkKind::Marker: return QColor(120, 160, 255);
    }
    return Qt::white;
}

QColor sectionColor(const QString& kind)
{
    if (kind == "hook") return QColor(220, 60, 60);
    if (kind == "build") return QColor(60, 120, 220);
    if (kind == "question") return QColor(200, 150, 30);
    if (kind == "turn") return QColor(150, 70, 200);
    if (kind == "resolution") return QColor(40, 160, 90);
    if (kind == "cta") return QColor(230, 110, 30);
    return QColor(110, 110, 110);
}

void drawOverlay(QPainter& p, QSize size, const Data& d, int frame)
{
    const double u = size.height() / 540.0;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    // Info box: timecode, frame number, section, tempo
    {
        QStringList lines{timecode(d, frame) + "   #" + QString::number(frame)};
        if (const Section* s = sectionAt(d, frame)) lines << sectionTitle(*s);
        if (!d.beats.empty()) {
            const int k = nearestBeat(d, frameS(d, frame));
            int inBar = 0; // beat of the bar (1..4)
            for (int j = k; j >= 0; --j)
                if (size_t(j) < d.bar.size() && d.bar[size_t(j)]) {
                    inBar = k - j + 1;
                    break;
                }
            lines << QString("%1 BPM").arg(std::lround(d.bpm)) + (inBar > 0 ? QString("  %1/4").arg((inBar - 1) % 4 + 1) : QString());
        }
        const QFont f = font(15 * u, true);
        p.setFont(f);
        const QFontMetrics fm(f);
        int w = 0;
        for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
        const QRectF box(8 * u, 8 * u, w + 14 * u, lines.size() * fm.height() + 8 * u);
        p.fillRect(box, QColor(0, 0, 0, 170));
        for (int i = 0; i < lines.size(); ++i)
            shadowText(p, QPointF(box.left() + 7 * u, box.top() + 4 * u + fm.ascent() + i * fm.height()), lines[i],
                       i == 1 && sectionAt(d, frame) ? sectionColor(sectionAt(d, frame)->kind).lighter(140) : Qt::white);
    }

    // Lamps
    p.setFont(font(10 * u, true));
    for (int i = 0; i < int(Lamp::Count); ++i) {
        const Lamp l = Lamp(i);
        const QRect r = lampRect(l, size);
        const bool lit = lampLit(d, l, frame);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(r, lit ? lampColor(l) : QColor(45, 45, 45));
        p.setPen(QPen(QColor(0, 0, 0), 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF label(r.left() - 10 * u, r.bottom() + 2 * u, r.width() + 20 * u, 13 * u);
        p.setPen(QColor(0, 0, 0, 200));
        p.drawText(label.translated(1, 1), Qt::AlignHCenter | Qt::AlignTop, lampName(l));
        p.setPen(lit ? lampColor(l) : QColor(200, 200, 200));
        p.drawText(label, Qt::AlignHCenter | Qt::AlignTop, lampName(l));
    }

    // Strip: the timeline around the playhead
    const QRect strip = stripRect(size);
    p.fillRect(strip, QColor(0, 0, 0, 165));
    const double now = frameS(d, frame);
    const double nameH = 14 * u, bandH = 14 * u;
    const QRectF names(strip.left(), strip.top(), strip.width(), nameH);
    Lane lane;
    lane.u = u;
    lane.r = QRectF(strip.left(), strip.top() + nameH, strip.width(), strip.height() - nameH - bandH);
    lane.from = now - kStripSpan;
    lane.to = now + kStripSpan;
    if (d.musicWave >= 0 && d.musicWave < d.waves.size()) drawWave(p, lane, d.waves[d.musicWave], QColor(150, 150, 150, 150));
    drawBeats(p, lane, d, 0.45, 230);
    drawMarks(p, lane, d, names, true);
    drawSections(p, lane, d, QRectF(strip.left(), strip.bottom() + 1 - bandH, strip.width(), bandH));
    p.setPen(QPen(Qt::white, 2 * u));
    p.drawLine(QPointF(lane.x(now), strip.top()), QPointF(lane.x(now), strip.bottom()));
}

// ---- Sheet

QVector<Tile> shots(const Data& d, int maxTiles)
{
    QVector<int> cuts{0};
    for (const Mark& m : d.marks)
        if (m.kind == MarkKind::Cut && m.frame > cuts.last() && m.frame < d.length) cuts << m.frame;
    QVector<Tile> all;
    for (int i = 0; i < cuts.size(); ++i) {
        Tile t;
        t.from = cuts[i];
        t.to = i + 1 < cuts.size() ? cuts[i + 1] : d.length;
        if (t.to <= t.from) continue;
        t.frame = t.from + (t.to - t.from) / 2;
        all << t;
    }
    if (all.size() <= maxTiles) return all;
    QVector<Tile> picked;
    for (int i = 0; i < maxTiles; ++i) picked << all[int((i + 0.5) * all.size() / maxTiles)];
    return picked;
}

QImage sheet(const Data& d, QVector<Tile>& tiles)
{
    const int W = kSheetWidth, margin = 12, gap = 8, cols = kSheetColumns;
    const double lengthS = std::max(1.0 / d.fps(), frameS(d, d.length));
    const int tileW = (W - 2 * margin - (cols - 1) * gap) / cols;
    const double aspect = d.format.width > 0 ? double(d.format.height) / d.format.width : 9.0 / 16;
    const int tileH = std::min(int(std::lround(tileW * aspect)), tileW * 2);
    const int labelH = 18, headerH = 44;
    const int tileRows = (int(tiles.size()) + cols - 1) / cols;
    const double span = std::max(20.0, std::ceil(lengthS / 15));
    const int rows = std::max(1, int(std::ceil(lengthS / span - 1e-9)));
    const int rulerH = 18, bandH = 18, namesH = 14, marksH = 56, waveH = 44, rowGap = 14;
    const int rowH = rulerH + bandH + namesH + marksH + int(d.waves.size()) * waveH + rowGap;
    const int H = headerH + tileRows * (tileH + labelH + gap) + gap + rows * rowH + margin;

    QImage out(W, H, QImage::Format_RGB32);
    out.fill(QColor(24, 24, 28));
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Header
    int counts[6] = {};
    for (const Mark& m : d.marks) ++counts[int(m.kind)];
    p.setFont(font(18, true));
    shadowText(p, QPointF(margin, 28), d.title.isEmpty() ? T("Prüf-Bogen") : d.title, Qt::white);
    p.setFont(font(13));
    QString info = QString("%1 (%2 s)").arg(timecode(d, d.length)).arg(lengthS, 0, 'f', 1);
    if (!d.beats.empty()) info += QString(" · %1 BPM (%2)").arg(d.bpm, 0, 'f', 1).arg(d.beatTrack);
    info += " · " + T("%1 Schnitte, %2 Treffer, %3 Kills, %4 Akzente, %5 Notizen")
                        .arg(counts[int(MarkKind::Cut)])
                        .arg(counts[int(MarkKind::Hit)])
                        .arg(counts[int(MarkKind::Kill)])
                        .arg(counts[int(MarkKind::Accent)])
                        .arg(counts[int(MarkKind::Note)]);
    shadowText(p, QPointF(margin + 360, 28), info, QColor(200, 200, 200));

    // Tiles
    for (int i = 0; i < tiles.size(); ++i) {
        Tile& t = tiles[i];
        const int x = margin + (i % cols) * (tileW + gap), y = headerH + (i / cols) * (tileH + labelH + gap);
        t.rect = QRect(x, y, tileW, tileH);
        p.fillRect(t.rect, Qt::black);
        if (!t.image.isNull()) {
            QRect target(QPoint(), t.image.size().scaled(t.rect.size(), Qt::KeepAspectRatio));
            target.moveCenter(t.rect.center());
            p.drawImage(target, t.image);
        }
        int hits = 0, kills = 0;
        for (const Mark& m : d.marks) {
            if (m.frame < t.from || m.frame >= t.to) continue;
            if (m.kind == MarkKind::Hit) ++hits;
            if (m.kind == MarkKind::Kill) ++kills;
        }
        p.setFont(font(11, true));
        if (hits + kills > 0) {
            const QString text = QString("▼%1 ▽%2").arg(hits).arg(kills);
            const QRect box(x + 4, y + 4, QFontMetrics(p.font()).horizontalAdvance(text) + 8, 16);
            p.fillRect(box, QColor(0, 0, 0, 180));
            p.setPen(markColor(MarkKind::Hit));
            p.drawText(box, Qt::AlignCenter, text);
        }
        p.setPen(QColor(70, 70, 70));
        p.drawRect(t.rect.adjusted(0, 0, -1, -1));
        p.setFont(font(11));
        const QString label = timecode(d, t.from) + "  " + t.clip;
        p.setPen(QColor(220, 220, 220));
        p.drawText(QRect(x, y + tileH + 1, tileW, labelH - 2), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                   QFontMetrics(p.font()).elidedText(label, Qt::ElideRight, tileW));
    }

    // Timeline rows
    int y = headerH + tileRows * (tileH + labelH + gap) + gap;
    for (int row = 0; row < rows; ++row) {
        Lane lane;
        lane.u = 1;
        lane.from = row * span;
        lane.to = lane.from + span;
        const double x0 = margin + 40, width = W - x0 - margin;
        // Ruler: a tick per second, timecode every 5 s
        const QRectF ruler(x0, y, width, rulerH);
        lane.r = ruler;
        p.setFont(font(10));
        for (int s = int(lane.from); s <= int(lane.to); ++s) {
            if (s > lengthS + 1e-9) break;
            const double x = lane.x(s);
            p.setPen(QColor(150, 150, 150));
            p.drawLine(QPointF(x, ruler.bottom() - (s % 5 == 0 ? 8 : 4)), QPointF(x, ruler.bottom()));
            if (s % 5 == 0) p.drawText(QPointF(x + 2, ruler.top() + 10), timecode(d, int(std::lround(s * d.fps()))));
        }
        const QRectF band(x0, y + rulerH, width, bandH);
        lane.r = band;
        drawSections(p, lane, d, band);
        const QRectF names(x0, band.bottom(), width, namesH);
        lane.r = QRectF(x0, names.bottom(), width, marksH);
        p.fillRect(lane.r, QColor(36, 36, 42));
        drawBeats(p, lane, d, 0.35, 200);
        // The end of the timeline
        if (lane.to > lengthS) p.fillRect(QRectF(lane.x(lengthS), y, lane.r.right() - lane.x(lengthS), rowH - rowGap), QColor(24, 24, 28));
        QRectF marksRect = lane.r;
        double wy = lane.r.bottom();
        for (const Wave& w : d.waves) {
            Lane wl = lane;
            wl.r = QRectF(x0, wy + 2, width, waveH - 4);
            p.fillRect(wl.r, QColor(32, 32, 38));
            drawBeats(p, wl, d, 0.0, 70);
            drawWave(p, wl, w, &w == &d.waves[std::max(0, d.musicWave)] && d.musicWave >= 0 ? QColor(90, 200, 255) : QColor(120, 220, 140));
            p.setFont(font(10, true));
            p.setPen(QColor(200, 200, 200));
            p.drawText(QRectF(margin, wl.r.top(), 38, wl.r.height()), Qt::AlignVCenter | Qt::AlignLeft, w.track);
            wy += waveH;
        }
        Lane ml = lane;
        ml.r = QRectF(x0, marksRect.top(), width, wy - marksRect.top());
        drawMarks(p, ml, d, names, true);
        p.setFont(font(10, true));
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRectF(margin, marksRect.top(), 38, marksRect.height()), Qt::AlignVCenter | Qt::AlignLeft, "V");
        y += rowH;
    }
    p.end();
    return out;
}

} // namespace ReviewSheet
