// SPDX-License-Identifier: AGPL-3.0-only
// Check before export, 0.17 part: pace (how often the picture changes), story (sections and strong moments) and the
// retention curve of a published video (core/ExportCheck.h)
#include "core/ExportCheck.h"

#include "core/I18n.h"
#include "core/JumpCut.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace ExportCheck {

namespace {

QString num(double v, int decimals = 1) { return QString::number(v, 'f', decimals).replace('.', I18n::decimalPoint()); }

Issue issue(Severity s, const char* check, const QString& message, int from = -1, int to = -1, int clip = 0)
{
    return {s, QString::fromLatin1(check), message, from, to, clip};
}

double secondsOf(int frames, const ProjectFormat& f) { return frames / f.rate.fps(); }
int framesOf(double seconds, const ProjectFormat& f) { return int(std::lround(seconds * f.rate.fps())); }

bool isPicture(const Clip& c) { return c.enabled && !c.isTitle(); }

// Short-form: vertical, or made for a platform (Shorts, Reels, TikTok)
bool shortForm(const ProjectFormat& f, const Options& o) { return isVertical(f) || !o.platform.isEmpty(); }

// The longest stretch between the bounds from, changes (inside), to
Stretch longestOf(const QVector<int>& frames, int from, int to)
{
    Stretch best{from, to};
    int last = from, longest = -1;
    for (int i = 0; i <= frames.size(); ++i) {
        const int next = i < frames.size() ? frames[i] : to;
        if (next - last > longest) {
            longest = next - last;
            best = {last, next};
        }
        last = next;
    }
    return best;
}

QString changeName(ChangeKind k)
{
    switch (k) {
    case ChangeKind::Cut: return T("Schnitt");
    case ChangeKind::Overlay: return T("Einblendung");
    case ChangeKind::Text: return T("Text");
    case ChangeKind::Motion: return T("Zoom/Bewegung");
    }
    return {};
}

} // namespace

const char* changeKindId(ChangeKind k)
{
    switch (k) {
    case ChangeKind::Cut: return "cut";
    case ChangeKind::Overlay: return "overlay";
    case ChangeKind::Text: return "text";
    case ChangeKind::Motion: return "motion";
    }
    return "cut";
}

// ---------- pace

QVector<Change> changes(const Timeline& tl, int from, int to, const ProjectFormat& f)
{
    QVector<Change> all, ends; // ends after starts: at a cut the clip that starts is the change
    const auto add = [&](int frame, ChangeKind kind, int clip) {
        if (frame > from && frame < to) all << Change{frame, kind, clip};
    };
    static constexpr AnimParam kMoving[] = {AnimParam::ZoomX, AnimParam::ZoomY,    AnimParam::PosX,
                                            AnimParam::PosY,  AnimParam::Rotation, AnimParam::TiltX,
                                            AnimParam::TiltY, AnimParam::GradeExposure};
    for (int t = 0; t < tl.video.size(); ++t) {
        const Track& track = tl.video[t];
        if (track.hidden) continue;
        for (const Clip& c : track.clips) {
            if (!c.enabled || c.end() <= from || c.start >= to) continue;
            const ChangeKind edge = c.isTitle() ? ChangeKind::Text : t == 0 ? ChangeKind::Cut : ChangeKind::Overlay;
            add(c.start, edge, c.id);
            if (c.end() > from && c.end() < to) ends << Change{c.end(), edge, c.id};
            // Keyframes where the value moves (a single one, or one between equal neighbours, holds still)
            // Also every animated value of the clip's compositing graph (a 3D text turning, a mask sweeping, a glitch
            // burst): on screen that is motion like a zoom
            for (auto it = c.keys.constBegin(); it != c.keys.constEnd(); ++it) {
                const AnimParam p = it.key();
                QString effect;
                const bool moving = std::find(std::begin(kMoving), std::end(kMoving), p) != std::end(kMoving)
                                    || (Keys::effectParamOf(p, &effect, nullptr) && effect == QLatin1String("comp"));
                if (!moving) continue;
                const KeyTrack& keys = *it;
                for (int i = 0; i < keys.size(); ++i) {
                    const bool before = i > 0 && std::abs(keys[i].value - keys[i - 1].value) > 1e-6;
                    const bool after = i + 1 < keys.size() && std::abs(keys[i + 1].value - keys[i].value) > 1e-6;
                    if (!before && !after) continue;
                    const int at = c.start + (keys[i].frame - c.in);
                    if (at >= c.start && at < c.end()) add(at, ChangeKind::Motion, c.id);
                }
            }
        }
    }
    for (const SubtitleTrack& st : tl.subtitles) {
        if (!st.enabled) continue;
        for (const SubtitleCue& cue : st.cues) add(cue.start, ChangeKind::Text, 0);
    }
    all += ends;
    std::stable_sort(all.begin(), all.end(), [](const Change& a, const Change& b) { return a.frame < b.frame; });
    // Close ones count once: an accent writes keyframes around its hit (rise, peak, landing within ~0.3 s), a cut
    // under a title edge is one change
    const int gap = std::max(1, framesOf(0.4, f));
    QVector<Change> out;
    for (const Change& c : all)
        if (out.isEmpty() || c.frame - out.last().frame >= gap) out << c;
    return out;
}

Pace pace(const Timeline& tl, int from, int to, const ProjectFormat& f, const Options& o)
{
    Pace p;
    if (to <= from) return p;
    p.changes = changes(tl, from, to, f);
    QVector<int> frames;
    for (const Change& c : p.changes) frames << c.frame;
    const double length = secondsOf(to - from, f);
    p.changesPerS = frames.size() / length;
    p.longest = longestOf(frames, from, to);
    const int maxStill = framesOf(o.maxStillS, f);
    int last = from;
    for (int i = 0; i <= frames.size(); ++i) {
        const int next = i < frames.size() ? frames[i] : to;
        if (next - last > maxStill) p.still << Stretch{last, next};
        last = next;
    }
    // At most ~60 steps of whole seconds
    p.curveStepS = std::max(1.0, std::ceil(length / 60));
    const int step = std::max(1, framesOf(p.curveStepS, f));
    for (int a = from; a < to; a += step) {
        const int b = std::min(to, a + step);
        const auto n = std::count_if(frames.begin(), frames.end(), [&](int x) { return x >= a && x < b; });
        p.curve << std::round(n / secondsOf(b - a, f) * 100) / 100;
    }
    for (const StorySection& s : tl.sections) {
        const int a = std::max(from, s.from), b = std::min(to, s.to);
        if (b <= a) continue;
        QVector<int> inside;
        for (int x : frames)
            if (x > a && x < b) inside << x;
        StorySection clipped = s;
        clipped.from = a;
        clipped.to = b;
        p.sections << SectionPace{clipped, inside.size() / secondsOf(b - a, f), longestOf(inside, a, b)};
    }
    return p;
}

QVector<Issue> paceIssues(const Pace& p, int from, int to, const ProjectFormat& f, const Options& o)
{
    QVector<Issue> out;
    if (p.still.isEmpty()) return out;
    if (isVertical(f) && secondsOf(to - from, f) <= o.hookMaxS) {
        for (const Stretch& s : p.still)
            out << issue(Severity::Warning, "pace_slow",
                         T("%1 s ohne Schnitt, Zoom oder Text – in kurzen Hochkant-Videos wischen Zuschauer hier "
                           "weiter; einen Schnitt, Zoom oder Text einbauen")
                             .arg(num(secondsOf(s.length(), f))),
                         s.from, s.to);
        return out;
    }
    const Stretch& l = p.longest;
    out << issue(Severity::Info, "pace_slow",
                 p.still.size() == 1
                     ? T("%1 s ohne Schnitt, Zoom oder Text").arg(num(secondsOf(l.length(), f)))
                     : T("%1 Stellen über %2 s ohne Schnitt, Zoom oder Text, die längste %3 s")
                           .arg(p.still.size())
                           .arg(num(o.maxStillS), num(secondsOf(l.length(), f))),
                 l.from, l.to);
    return out;
}

QVector<int> breakPoints(const Timeline& tl, const Stretch& s, const ProjectFormat& f, const Options& o)
{
    QVector<int> out;
    const int maxStill = std::max(1, framesOf(o.maxStillS, f));
    if (s.length() <= maxStill) return out;
    // Pieces of at most 90 % of the allowance: an accent's change starts a frame before its hit
    const int pieces = int(std::ceil(s.length() / (0.9 * maxStill)));
    const auto covered = [&](int frame) {
        for (const Track& t : tl.video) {
            if (t.hidden) continue;
            for (const Clip& c : t.clips)
                if (isPicture(c) && c.start <= frame && frame < c.end()) return true;
        }
        return false;
    };
    for (int j = 1; j < pieces; ++j) {
        const int at = s.from + int(std::lround(double(s.length()) * j / pieces));
        if (covered(at)) out << at;
    }
    return out;
}

// ---------- story

QVector<int> loudMoments(const std::vector<float>& framePeaks, int offset, double fps)
{
    QVector<int> out;
    const int n = int(framePeaks.size());
    const int window = std::max(1, int(std::lround(0.5 * fps))), skip = std::max(1, int(std::lround(0.25 * fps)));
    std::vector<double> db(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) db[size_t(i)] = 20 * std::log10(std::max(1e-6f, framePeaks[size_t(i)]));
    std::vector<double> before;
    for (int i = window; i < n; ++i) {
        if (db[size_t(i)] < -24) continue;
        before.assign(db.begin() + (i - window), db.begin() + i);
        std::nth_element(before.begin(), before.begin() + window / 2, before.end());
        if (db[size_t(i)] - before[size_t(window / 2)] < 12) continue;
        out << offset + i;
        i += skip - 1;
    }
    return out;
}

int firstText(const Timeline& tl, int from, int to)
{
    int first = -1;
    const auto take = [&](int start, int end) {
        if (end <= from || start >= to) return;
        const int at = std::max(start, from);
        if (first < 0 || at < first) first = at;
    };
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips)
            if (c.enabled && c.isTitle()) take(c.start, c.end());
    }
    for (const SubtitleTrack& st : tl.subtitles)
        if (st.enabled)
            for (const SubtitleCue& cue : st.cues) take(cue.start, cue.end);
    return first;
}

QVector<int> strongMoments(const Timeline& tl, int from, int to, const ProjectFormat& f,
                           const std::vector<float>* framePeaks, const QVector<int>& words, QString* by)
{
    QVector<QPair<int, QString>> all;
    if (framePeaks)
        for (int x : loudMoments(*framePeaks, from, f.rate.fps()))
            if (x < to) all << qMakePair(x, QString("loud"));
    int word = -1;
    for (int w : words)
        if (w >= from && w < to && (word < 0 || w < word)) word = w;
    if (word >= 0) all << qMakePair(word, QString("word"));
    if (const int text = firstText(tl, from, to); text >= 0) all << qMakePair(text, QString("text"));
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (by) *by = all.isEmpty() ? QString() : all.first().second;
    QVector<int> out;
    for (const auto& x : all) out << x.first;
    return out;
}

QVector<Issue> story(const Timeline& tl, int from, int to, const ProjectFormat& f, QVector<int> strong,
                     bool strongKnown, const QVector<int>& words, const Options& o)
{
    QVector<Issue> out;
    if (to <= from) return out;
    const double length = secondsOf(to - from, f);
    const bool isShort = shortForm(f, o);

    // Sections in the range, clipped to it
    QVector<StorySection> sections;
    for (StorySection s : tl.sections) {
        s.from = std::max(from, s.from);
        s.to = std::min(to, s.to);
        if (s.to > s.from) sections << s;
    }

    // The first strong moment: a loud hit, the first word, a text
    if (const int text = firstText(tl, from, to); text >= 0) strong << text;
    if (from == 0 || o.rangeIsVideo) {
        const double limit = isShort ? o.hookLateShortS : o.hookLateS;
        const Severity sev = isShort && length <= o.hookMaxS ? Severity::Warning : Severity::Info;
        int first = -1;
        for (int s : strong)
            if (s >= from && s < to && (first < 0 || s < first)) first = s;
        if (length > limit) {
            if (first >= 0 && secondsOf(first - from, f) > limit)
                out << issue(sev, "story_hook_late",
                             T("Der erste starke Moment (Treffer, lauter Moment, erstes Wort oder Text) kommt erst nach "
                               "%1 s – in den ersten %2 s entscheidet sich, wer bleibt; ihn nach vorn holen oder als "
                               "Teaser zeigen")
                                 .arg(num(secondsOf(first - from, f)), num(limit, 0)),
                             from, first);
            else if (first < 0 && strongKnown)
                out << issue(sev, "story_hook_late",
                             T("Kein starker Moment (Treffer, lauter Moment, Wort oder Text) gefunden – der Anfang "
                               "braucht einen Hook"),
                             from, std::min(to, from + framesOf(limit, f)));
        }
    }

    const auto endsNear = [&](int frame) { return to - frame <= std::max(1, framesOf(0.5, f)); };
    // A story planned with sections (more than a hook and free ones, e.g. the moments `short` marks): it asks for a
    // call-to-action section. Else one can only be guessed: text or speech near the end
    const bool planned = std::any_of(sections.begin(), sections.end(), [](const StorySection& s) {
        return s.kind != StoryKind::Hook && s.kind != StoryKind::Free;
    });
    if (!planned) {
        if (!o.platform.isEmpty() && length > 6) {
            const int tail = to - std::max(framesOf(3, f), (to - from) * 15 / 100);
            const bool text = firstText(tl, tail, to) >= 0;
            const bool spoken = std::any_of(words.begin(), words.end(), [&](int w) { return w >= tail && w < to; });
            if (!text && !spoken)
                out << issue(Severity::Info, "story_no_cta",
                             T("Am Ende weder Text noch Sprache – ein Aufruf (folgen, kommentieren, Link) fehlt wohl"),
                             tail, to);
        }
        if (sections.isEmpty()) return out;
    }

    const auto firstOf = [&](StoryKind k) -> const StorySection* {
        for (const StorySection& s : sections)
            if (s.kind == k) return &s;
        return nullptr;
    };
    const auto at = [&](const StorySection& s) { return num(secondsOf(s.from, f)); };

    // Hook at the start
    if (from == 0 || o.rangeIsVideo) {
        const StorySection* hook = firstOf(StoryKind::Hook);
        if (!hook || hook->from > from + framesOf(0.5, f))
            out << issue(Severity::Warning, "story_no_hook",
                         hook ? T("Der Hook beginnt erst bei %1 s – an den Anfang legen").arg(at(*hook))
                              : T("Kein Hook-Abschnitt am Anfang – die ersten Sekunden entscheiden, ob jemand bleibt"),
                         from, hook ? hook->from : std::min(to, from + framesOf(o.hookLateShortS, f)));
    }

    // Resolution: late, or no room left for the call to action
    const StorySection* cta = firstOf(StoryKind::Cta);
    if (const StorySection* r = firstOf(StoryKind::Resolution)) {
        const double share = double(r->from - from) / (to - from);
        bool ctaAfter = false;
        for (const StorySection& s : sections) ctaAfter |= s.kind == StoryKind::Cta && s.from >= r->from;
        if (share > 0.9)
            out << issue(Severity::Warning, "story_resolution_late",
                         T("Die Auflösung beginnt erst bei %1 % der Länge – kaum Zeit, sie wirken zu lassen")
                             .arg(int(std::lround(share * 100))),
                         r->from, r->to);
        else if (endsNear(r->to) && !ctaAfter && (cta || !o.platform.isEmpty()))
            out << issue(Severity::Info, "story_resolution_late",
                         T("Die Auflösung läuft bis zum Ende – kein Platz mehr für den Aufruf"), r->from, r->to);
    }

    // A question needs a turn or a resolution after it
    for (const StorySection& q : sections) {
        if (q.kind != StoryKind::Question) continue;
        const bool answered = std::any_of(sections.begin(), sections.end(), [&](const StorySection& s) {
            return (s.kind == StoryKind::Turn || s.kind == StoryKind::Resolution) && s.from > q.from;
        });
        if (!answered)
            out << issue(Severity::Warning, "story_open_question",
                         T("Die Frage bei %1 s bleibt offen – danach fehlt eine Wendung oder Auflösung").arg(at(q)), q.from,
                         q.to);
    }

    if (!cta && planned)
        out << issue(Severity::Info, "story_no_cta",
                     T("Kein Aufruf-Abschnitt – am Ende sagen, was Zuschauer tun sollen (folgen, kommentieren, Link)"));

    // Time without a section
    QVector<StorySection> sorted = sections;
    std::sort(sorted.begin(), sorted.end(), [](const StorySection& a, const StorySection& b) { return a.from < b.from; });
    const int minGap = std::max(1, framesOf(0.5, f));
    int covered = from;
    const auto gap = [&](int a, int b) {
        if (b - a >= minGap)
            out << issue(Severity::Info, "story_gap",
                         T("%1 s ohne Abschnitt").arg(num(secondsOf(b - a, f))), a, b);
    };
    for (const StorySection& s : sorted) {
        gap(covered, s.from);
        covered = std::max(covered, s.to);
    }
    gap(covered, to);
    return out;
}

// ---------- jump cuts

QVector<Issue> jumpCuts(const Timeline& tl, int from, int to, const ProjectFormat& f)
{
    QVector<Issue> out;
    int count = 0;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const JumpCut::Jump& j : JumpCut::find(t, framesOf(0.3, f))) {
            if (j.at <= from || j.at >= to) continue;
            const Clip* a = TimelineOps::findClip(tl, j.left);
            const Clip* b = TimelineOps::findClip(tl, j.right);
            if (!a || !b || !JumpCut::sameFraming(*a, *b)) continue;
            // Covered at the cut by a picture on a track above (B-roll): nobody sees the jump
            bool covered = false;
            for (int u = int(&t - tl.video.data()) + 1; u < tl.video.size() && !covered; ++u)
                if (!tl.video[u].hidden)
                    for (const Clip& c : tl.video[u].clips)
                        covered |= c.enabled && !c.isTitle() && c.start <= j.at && c.end() > j.at;
            if (covered) continue;
            if (++count <= 20)
                out << issue(Severity::Info, "jump_cut",
                             T("Sprung im Bild bei %1 s: dieselbe Aufnahme, %2 s ausgelassen – abwechselnd "
                               "heranzoomen lässt ihn wie einen Kamerawechsel wirken")
                                 .arg(num(secondsOf(j.at - from, f)), num(secondsOf(std::abs(j.skipped), f))),
                             j.at, j.at + 1, j.right);
        }
    }
    if (count > 20)
        out << issue(Severity::Info, "jump_cut", T("%1 weitere Sprünge im Bild").arg(count - 20));
    return out;
}

// ---------- retention

namespace {

// Seconds of "12.5", "12,5", "12s", "1:07", "0:01:07.5"; nullopt = not a time
std::optional<double> timeOf(QString s)
{
    s = s.trimmed().remove('"');
    if (s.endsWith('s')) s.chop(1);
    s.replace(',', '.');
    if (s.isEmpty()) return std::nullopt;
    const QStringList parts = s.split(':');
    if (parts.size() > 3) return std::nullopt;
    double total = 0;
    for (int i = 0; i < parts.size(); ++i) {
        bool ok = false;
        const double v = parts[i].trimmed().toDouble(&ok);
        if (!ok || v < 0 || !std::isfinite(v)) return std::nullopt;
        total = total * 60 + v;
    }
    return total;
}

std::optional<double> percentOf(QString s)
{
    s = s.trimmed().remove('"').remove('%').trimmed();
    s.replace(',', '.');
    bool ok = false;
    const double v = s.toDouble(&ok);
    if (!ok || !std::isfinite(v) || v < 0) return std::nullopt;
    return v;
}

// Retention at time t, linear between the points (clamped at the ends)
double valueAt(const QVector<RetentionPoint>& c, double t)
{
    if (t <= c.first().seconds) return c.first().percent;
    for (int i = 1; i < c.size(); ++i)
        if (t <= c[i].seconds) {
            const double span = c[i].seconds - c[i - 1].seconds;
            const double k = span > 0 ? (t - c[i - 1].seconds) / span : 1;
            return c[i - 1].percent + (c[i].percent - c[i - 1].percent) * k;
        }
    return c.last().percent;
}

} // namespace

QVector<RetentionPoint> retentionFromCsv(const QString& text, double lengthS, QString* error)
{
    const auto failed = [&](const QString& why) {
        if (error) *error = why;
        return QVector<RetentionPoint>{};
    };
    QStringList lines;
    QString body = text;
    if (body.startsWith(QChar(0xfeff))) body.remove(0, 1); // a byte order mark (Excel's UTF-8 CSV)
    for (const QString& l : body.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts))
        if (!l.trimmed().isEmpty()) lines << l;
    if (lines.isEmpty()) return failed("the file is empty");
    const QString& probe = lines.first();
    const QChar sep = probe.contains('\t') ? QChar('\t') : probe.contains(';') ? QChar(';') : QChar(',');
    // Cells split at the separator outside quotes ("0,5","85,3": decimal commas in a comma file), quotes removed
    const auto fields = [&](const QString& l) {
        QStringList out;
        QString cell;
        bool quoted = false;
        for (int i = 0; i < l.size(); ++i) {
            const QChar ch = l[i];
            if (ch == '"') {
                if (quoted && i + 1 < l.size() && l[i + 1] == '"') cell += l[++i]; // "" inside quotes
                else quoted = !quoted;
            } else if (ch == sep && !quoted) {
                out << cell;
                cell.clear();
            } else {
                cell += ch;
            }
        }
        out << cell;
        return out;
    };

    int timeCol = 0, valueCol = 1;
    bool position = false;
    int first = 0;
    if (const QStringList head = fields(lines.first()); !head.isEmpty() && !timeOf(head[0])) {
        first = 1;
        const QString t = head[0].toLower();
        position = t.contains('%') || t.contains("position") || t.contains("posición") || t.contains("pozycja");
        // The absolute retention (share of all viewers) before a relative one (against similar videos)
        const auto column = [&](std::initializer_list<const char*> words) {
            for (int i = 1; i < head.size(); ++i)
                for (const char* w : words)
                    if (head[i].toLower().contains(QLatin1String(w))) return i;
            return -1;
        };
        if (const int c = column({"absolute", "audience", "zuschauer", "viewers"}); c > 0) valueCol = c;
        else if (const int r = column({"retention", "bindung"}); r > 0) valueCol = r;
    }
    QVector<RetentionPoint> out;
    for (int i = first; i < lines.size(); ++i) {
        const QStringList cells = fields(lines[i]);
        if (cells.size() <= std::max(timeCol, valueCol)) continue;
        const auto t = position ? percentOf(cells[timeCol]) : timeOf(cells[timeCol]);
        const auto v = percentOf(cells[valueCol]);
        if (t && v) out << RetentionPoint{*t, *v};
    }
    if (out.size() < 2) return failed("no rows of time and retention found (need at least two)");
    std::stable_sort(out.begin(), out.end(),
                     [](const RetentionPoint& a, const RetentionPoint& b) { return a.seconds < b.seconds; });
    const auto maxOf = [&](auto get) {
        double m = 0;
        for (const RetentionPoint& p : out) m = std::max(m, get(p));
        return m;
    };
    if (position) {
        if (lengthS <= 0) return failed("positions in % need the video's length");
        const double scale = maxOf([](const RetentionPoint& p) { return p.seconds; }) <= 1.0 ? 1.0 : 100.0;
        for (RetentionPoint& p : out) p.seconds = p.seconds / scale * lengthS;
    }
    if (maxOf([](const RetentionPoint& p) { return p.percent; }) <= 1.5)
        for (RetentionPoint& p : out) p.percent *= 100;
    return out;
}

QVector<Issue> retention(const QVector<RetentionPoint>& curve, const QVector<Change>& changes, int from,
                         const ProjectFormat& f)
{
    QVector<Issue> out;
    if (curve.size() < 2) return out;
    constexpr double kWindow = 2, kStart = 1, kMinFall = 5;
    // The fall over the next two seconds at every point after the first second
    QVector<QPair<double, double>> falls; // (time, fall)
    for (const RetentionPoint& p : curve)
        if (p.seconds >= kStart && p.seconds + kWindow <= curve.last().seconds + 1e-9)
            falls << qMakePair(p.seconds, p.percent - valueAt(curve, p.seconds + kWindow));
    if (falls.isEmpty()) return out;
    QVector<double> sorted;
    for (const auto& fl : falls) sorted << fl.second;
    std::sort(sorted.begin(), sorted.end());
    const double usual = std::max(0.0, sorted[sorted.size() / 2]);
    const double threshold = std::max(kMinFall, 3 * usual);

    for (int i = 0; i < falls.size();) {
        if (falls[i].second < threshold) {
            ++i;
            continue;
        }
        int j = i;
        while (j + 1 < falls.size() && falls[j + 1].second >= threshold) ++j;
        const double a = falls[i].first, b = falls[j].first + kWindow;
        // The drop itself: the end of the steepest step between two points of the curve in [a, b]
        double steepest = -1, dropAt = b;
        for (int k = 1; k < curve.size(); ++k) {
            const RetentionPoint &p = curve[k - 1], &q = curve[k];
            if (q.seconds <= a || p.seconds >= b || q.seconds <= p.seconds) continue;
            const double slope = (p.percent - q.percent) / (q.seconds - p.seconds);
            if (slope > steepest) {
                steepest = slope;
                dropAt = q.seconds;
            }
        }
        const double fall = valueAt(curve, a) - valueAt(curve, b);
        const int dropFrame = from + framesOf(dropAt, f);
        const Change* before = nullptr;
        for (const Change& c : changes)
            if (c.frame <= dropFrame) before = &c;
        Issue x = issue(Severity::Info, "retention_drop", QString(), from + framesOf(a, f), from + framesOf(b, f));
        if (before) {
            x.at = before->frame;
            x.clip = before->clip;
            x.message = T("Bei %1 s springen %2 Prozentpunkte der Zuschauer ab – davor %3 bei %4 s")
                            .arg(num(dropAt), num(fall), changeName(before->kind), num(secondsOf(before->frame - from, f)));
        } else {
            x.message = T("Bei %1 s springen %2 Prozentpunkte der Zuschauer ab").arg(num(dropAt), num(fall));
        }
        out << x;
        i = j + 1;
    }
    return out;
}

} // namespace ExportCheck
